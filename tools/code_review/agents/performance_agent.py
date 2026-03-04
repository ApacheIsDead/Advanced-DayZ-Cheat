"""Performance Agent - Reviews code for performance issues and resource management."""

import re
from .base_agent import BaseAgent
from ..models import Finding, Severity, AgentReview, Verdict


class PerformanceAgent(BaseAgent):
    NAME = "PerformanceAgent"
    FOCUS = "Performance bottlenecks, memory management, and resource efficiency"

    # Memory management anti-patterns
    MEMORY_PATTERNS = {
        r"\bmalloc\s*\([^)]+\)(?:(?!free).)*$": (
            "malloc without nearby free",
            "Ensure every malloc has a corresponding free on all code paths",
            Severity.MEDIUM,
        ),
        r"\bnew\s+\w+(?:\[[^\]]*\])?\s*;": (
            "Raw new allocation",
            "Prefer smart pointers (unique_ptr/shared_ptr) over raw new",
            Severity.LOW,
        ),
    }

    # Inefficient patterns
    INEFFICIENCY_PATTERNS = {
        r"std::endl": (
            "std::endl forces flush",
            "Use '\\n' instead of std::endl unless flush is explicitly needed",
            Severity.LOW,
        ),
        r"\.size\(\)\s*>\s*0": (
            "Verbose emptiness check",
            "Use !container.empty() instead of .size() > 0",
            Severity.INFO,
        ),
        r"\.find\([^)]+\)\s*!=\s*\w+\.end\(\)": (
            "Find-then-access pattern",
            "Consider using count() or contains() for existence checks",
            Severity.INFO,
        ),
    }

    # Spin-wait and busy-loop patterns
    SPINWAIT_PATTERNS = {
        r"while\s*\(\s*true\s*\)\s*\{[^}]*Sleep\s*\(\s*0\s*\)": (
            "Busy-wait with Sleep(0)",
            "Use proper synchronization (events, semaphores) instead of spin-waiting",
            Severity.HIGH,
        ),
        r"while\s*\([^)]*\)\s*\{\s*\}": (
            "Empty spin-loop",
            "Empty spin-loops waste CPU — use synchronization primitives",
            Severity.HIGH,
        ),
        r"while\s*\(\s*!\s*\w+\s*\)\s*;": (
            "Inline spin-wait",
            "Spin-waiting wastes CPU cycles — use event-based waiting",
            Severity.MEDIUM,
        ),
    }

    # Kernel-specific performance patterns
    KERNEL_PERF_PATTERNS = {
        r"KeDelayExecutionThread": (
            "Thread sleep in kernel",
            "Kernel thread sleeping can block critical paths — use deferred work items",
            Severity.MEDIUM,
        ),
        r"MmAllocateContiguousMemory": (
            "Contiguous memory allocation",
            "Contiguous allocations are expensive — allocate early and reuse",
            Severity.LOW,
        ),
        r"RtlCopyMemory\s*\([^,]+,\s*[^,]+,\s*(?:sizeof|0x[0-9a-fA-F]{4,})": (
            "Large memory copy",
            "Large memory copies in hot paths affect performance — consider zero-copy approaches",
            Severity.LOW,
        ),
    }

    # Stack allocation warnings
    STACK_PATTERNS = {
        r"\w+\s+\w+\[\s*(?:[1-9]\d{3,}|0x[4-9a-fA-F][0-9a-fA-F]{2,})\s*\]": (
            "Large stack allocation",
            "Large stack arrays risk overflow — use heap allocation for buffers > 4KB",
            Severity.MEDIUM,
        ),
    }

    def review_file(self, filepath: str, content: str, lines: list[str]) -> list[Finding]:
        findings = []

        pattern_groups = [
            ("Memory Management", self.MEMORY_PATTERNS),
            ("Inefficiency", self.INEFFICIENCY_PATTERNS),
            ("Spin-Wait", self.SPINWAIT_PATTERNS),
            ("Kernel Performance", self.KERNEL_PERF_PATTERNS),
            ("Stack Usage", self.STACK_PATTERNS),
        ]

        for category, patterns in pattern_groups:
            for pattern, (name, suggestion, severity) in patterns.items():
                for match in re.finditer(pattern, content, re.MULTILINE):
                    line_num = content[:match.start()].count("\n") + 1
                    findings.append(Finding(
                        file=filepath, line=line_num, severity=severity,
                        category=category, agent=self.NAME,
                        description=name, suggestion=suggestion,
                    ))

        # --- Repeated allocation in loops ---
        findings.extend(self._check_loop_allocations(filepath, content, lines))

        # --- Unnecessary copies (pass-by-value for large types) ---
        findings.extend(self._check_pass_by_value(filepath, content))

        # --- Missing const on pointer parameters ---
        findings.extend(self._check_const_correctness(filepath, content))

        return findings

    def _check_loop_allocations(self, filepath: str, content: str, lines: list[str]) -> list[Finding]:
        findings = []
        in_loop = False
        loop_depth = 0
        loop_start = 0

        for i, line in enumerate(lines, 1):
            stripped = line.strip()
            if re.match(r"(for|while|do)\s*[\({]", stripped):
                if not in_loop:
                    loop_start = i
                in_loop = True
                loop_depth += 1

            if in_loop:
                loop_depth += line.count("{") - line.count("}")
                if loop_depth <= 0:
                    in_loop = False
                    loop_depth = 0
                    continue

                # Check for allocations inside loops
                alloc_funcs = ["malloc", "calloc", "realloc", "new ", "ExAllocatePool",
                               "VirtualAlloc", "HeapAlloc", "MmAllocate"]
                for func in alloc_funcs:
                    if func in stripped:
                        findings.append(Finding(
                            file=filepath, line=i, severity=Severity.MEDIUM,
                            category="Loop Allocation", agent=self.NAME,
                            description=f"Memory allocation ({func.strip()}) inside loop (started line {loop_start})",
                            suggestion="Hoist allocation outside loop or use a pre-allocated buffer pool",
                        ))
        return findings

    def _check_pass_by_value(self, filepath: str, content: str) -> list[Finding]:
        findings = []
        large_types = [
            "std::string", "string", "vector", "map", "set", "unordered_map",
            "wstring", "UNICODE_STRING", "D3DMATRIX", "Matrix4x4",
        ]
        for t in large_types:
            escaped = re.escape(t)
            pattern = rf"(?:void|int|bool|auto|NTSTATUS)\s+\w+\s*\([^)]*(?<![&*]){escaped}\s+\w+"
            for match in re.finditer(pattern, content):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=Severity.LOW,
                    category="Unnecessary Copy", agent=self.NAME,
                    description=f"'{t}' passed by value — may cause unnecessary copy",
                    suggestion=f"Pass '{t}' by const reference (const {t}&) to avoid copies",
                ))
        return findings

    def _check_const_correctness(self, filepath: str, content: str) -> list[Finding]:
        findings = []
        # Look for pointer parameters that could be const
        pattern = r"(\w+)\s*\*\s+(\w+)\s*[,)]"
        skip_types = {"void", "PVOID", "LPVOID", "char", "PCHAR", "PUCHAR", "HANDLE"}
        seen = set()

        for match in re.finditer(pattern, content):
            type_name = match.group(1)
            param_name = match.group(2)
            if type_name in skip_types:
                continue
            key = f"{type_name}*{param_name}"
            if key in seen:
                continue
            seen.add(key)

            # Check if the pointed-to data is modified
            write_pattern = rf"\*{re.escape(param_name)}\s*="
            arrow_write = rf"{re.escape(param_name)}\s*->\s*\w+\s*="
            if not re.search(write_pattern, content) and not re.search(arrow_write, content):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=Severity.INFO,
                    category="Const Correctness", agent=self.NAME,
                    description=f"Pointer parameter '{param_name}' ({type_name}*) could be const",
                    suggestion=f"Mark as 'const {type_name}*' if data is not modified",
                ))
        return findings

    def compute_verdict(self, review: AgentReview) -> Verdict:
        if review.critical_count > 0:
            return Verdict.FAIL
        if review.high_count >= 2 or len(review.findings) >= 8:
            return Verdict.WARN
        return Verdict.PASS
