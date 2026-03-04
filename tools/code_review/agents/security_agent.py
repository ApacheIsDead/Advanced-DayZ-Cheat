"""Security Agent - Reviews code for security vulnerabilities and unsafe patterns."""

import re
from pathlib import Path
from .base_agent import BaseAgent
from ..models import Finding, Severity, AgentReview, Verdict


class SecurityAgent(BaseAgent):
    NAME = "SecurityAgent"
    FOCUS = "Security vulnerabilities, unsafe memory operations, and attack surface analysis"

    # Unsafe C functions that have safer alternatives
    UNSAFE_FUNCTIONS = {
        r"\bstrcpy\s*\(": ("strcpy", "Use strncpy or strcpy_s to prevent buffer overflows"),
        r"\bstrcat\s*\(": ("strcat", "Use strncat or strcat_s to prevent buffer overflows"),
        r"\bsprintf\s*\(": ("sprintf", "Use snprintf or sprintf_s to prevent buffer overflows"),
        r"\bgets\s*\(": ("gets", "Use fgets — gets has no bounds checking"),
        r"\bscanf\s*\(": ("scanf", "Use fgets+sscanf or scanf_s with width limits"),
        r"\bvsprintf\s*\(": ("vsprintf", "Use vsnprintf to prevent buffer overflows"),
        r"\bwcscpy\s*\(": ("wcscpy", "Use wcsncpy or wcscpy_s to prevent buffer overflows"),
        r"\b_tcscpy\s*\(": ("_tcscpy", "Use _tcsncpy to prevent buffer overflows"),
    }

    # Patterns indicating potential memory safety issues
    MEMORY_PATTERNS = {
        r"\bmemcpy\s*\([^)]*,\s*[^)]*,\s*sizeof\s*\(\s*\*": (
            "Suspicious memcpy size",
            "Verify memcpy size matches destination buffer capacity",
            Severity.MEDIUM,
        ),
        r"\bfree\s*\([^)]+\)(?!.*=\s*NULL)": (
            "Free without NULL assignment",
            "Set pointer to NULL after free to prevent use-after-free",
            Severity.LOW,
        ),
        r"ExAllocatePool\b": (
            "Deprecated ExAllocatePool",
            "Use ExAllocatePool2 or ExAllocatePoolWithTag for tracked allocations",
            Severity.MEDIUM,
        ),
        r"ExAllocatePoolWithTag\s*\(\s*NonPagedPool": (
            "NonPagedPool allocation",
            "Consider NonPagedPoolNx to prevent code execution from pool memory",
            Severity.MEDIUM,
        ),
    }

    # Patterns for hardcoded sensitive data
    HARDCODED_PATTERNS = {
        r"(?:password|passwd|pwd)\s*=\s*[\"'][^\"']+[\"']": (
            "Hardcoded password",
            "Use secure credential storage instead of hardcoded passwords",
            Severity.CRITICAL,
        ),
        r"(?:api[_-]?key|apikey)\s*=\s*[\"'][^\"']+[\"']": (
            "Hardcoded API key",
            "Use environment variables or secure vault for API keys",
            Severity.CRITICAL,
        ),
        r"[A-Za-z]:\\\\Users\\\\[^\"'\\\\]+": (
            "Hardcoded user path",
            "Use relative paths or environment variables instead of hardcoded paths",
            Severity.LOW,
        ),
    }

    # Kernel-specific security patterns
    KERNEL_PATTERNS = {
        r"ProbeForRead|ProbeForWrite": (
            "User-buffer probing",
            "Ensure all user-mode buffer access is wrapped in try/except blocks",
            Severity.HIGH,
        ),
        r"__try\s*\{": (
            "SEH usage (informational)",
            "Verify exception handlers cover all user-mode buffer accesses",
            Severity.INFO,
        ),
        r"MmMapIoSpace": (
            "Physical memory mapping",
            "Physical memory mapping can bypass OS security — ensure strict validation",
            Severity.HIGH,
        ),
        r"ZwMapViewOfSection|NtMapViewOfSection": (
            "Section mapping",
            "Validate section mapping permissions and target process carefully",
            Severity.MEDIUM,
        ),
        r"KeInsertQueueApc|KeInitializeApc": (
            "APC injection",
            "APC injection into user processes is a high-privilege operation — validate target",
            Severity.HIGH,
        ),
        r"IoCreateDevice\s*\(": (
            "Device object creation",
            "Ensure proper security descriptor and exclusive access flags",
            Severity.MEDIUM,
        ),
    }

    # Race condition / synchronization patterns
    RACE_PATTERNS = {
        r"InterlockedExchange|InterlockedCompareExchange": (
            "Interlocked operation (informational)",
            "Verify surrounding code doesn't have TOCTOU issues",
            Severity.INFO,
        ),
        r"while\s*\(\s*\*\s*\w+\s*(?:==|!=)": (
            "Spin-wait on shared variable",
            "Spin-waits without memory barriers can cause stale reads on some architectures",
            Severity.MEDIUM,
        ),
    }

    def review_file(self, filepath: str, content: str, lines: list[str]) -> list[Finding]:
        findings = []

        for pattern, (name, suggestion) in self.UNSAFE_FUNCTIONS.items():
            for match in re.finditer(pattern, content):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=Severity.HIGH,
                    category="Unsafe Function", agent=self.NAME,
                    description=f"Use of unsafe function '{name}'",
                    suggestion=suggestion,
                ))

        for pattern, (name, suggestion, severity) in self.MEMORY_PATTERNS.items():
            for match in re.finditer(pattern, content):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=severity,
                    category="Memory Safety", agent=self.NAME,
                    description=name, suggestion=suggestion,
                ))

        for pattern, (name, suggestion, severity) in self.HARDCODED_PATTERNS.items():
            for match in re.finditer(pattern, content, re.IGNORECASE):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=severity,
                    category="Hardcoded Secrets", agent=self.NAME,
                    description=name, suggestion=suggestion,
                ))

        for pattern, (name, suggestion, severity) in self.KERNEL_PATTERNS.items():
            for match in re.finditer(pattern, content):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=severity,
                    category="Kernel Security", agent=self.NAME,
                    description=name, suggestion=suggestion,
                ))

        for pattern, (name, suggestion, severity) in self.RACE_PATTERNS.items():
            for match in re.finditer(pattern, content):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=severity,
                    category="Concurrency", agent=self.NAME,
                    description=name, suggestion=suggestion,
                ))

        # Check for unchecked return values from critical functions
        critical_funcs = [
            "ExAllocatePool", "MmAllocateContiguousMemory", "IoAllocateMdl",
            "MmMapLockedPagesSpecifyCache", "MmMapIoSpace",
        ]
        for func in critical_funcs:
            pattern = rf"{func}\s*\([^;]+;\s*\n\s*(?!if\s*\()"
            for match in re.finditer(pattern, content):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=Severity.HIGH,
                    category="Unchecked Return", agent=self.NAME,
                    description=f"Return value of {func} may not be checked",
                    suggestion=f"Always check {func} return for NULL before use",
                ))

        return findings

    def compute_verdict(self, review: AgentReview) -> Verdict:
        if review.critical_count > 0 or review.high_count >= 3:
            return Verdict.FAIL
        if review.high_count > 0 or len(review.findings) >= 5:
            return Verdict.WARN
        return Verdict.PASS
