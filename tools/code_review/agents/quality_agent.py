"""Quality Agent - Reviews code for quality, readability, and maintainability issues."""

import re
from .base_agent import BaseAgent
from ..models import Finding, Severity, AgentReview, Verdict


class QualityAgent(BaseAgent):
    NAME = "QualityAgent"
    FOCUS = "Code quality, readability, maintainability, and correctness"

    MAX_FUNCTION_LINES = 60
    MAX_NESTING_DEPTH = 5
    MAX_LINE_LENGTH = 150
    MAX_PARAMS = 6

    # Patterns for code smells
    CODE_SMELLS = {
        r"//\s*TODO\b": ("TODO comment", "Resolve or track TODOs in issue tracker", Severity.LOW),
        r"//\s*FIXME\b": ("FIXME comment", "FIXME indicates a known bug — prioritize fixing", Severity.MEDIUM),
        r"//\s*HACK\b": ("HACK comment", "HACK indicates technical debt — plan refactoring", Severity.MEDIUM),
        r"//\s*XXX\b": ("XXX marker", "XXX indicates problematic code — investigate", Severity.MEDIUM),
    }

    # Patterns suggesting dead or debug code
    DEBUG_PATTERNS = {
        r"\bprintf\s*\(\s*\"debug": ("Debug printf left in code", "Remove debug print statements", Severity.LOW),
        r"\bDbgPrint\s*\(": ("DbgPrint in code", "Wrap DbgPrint in conditional compilation (#if DBG)", Severity.LOW),
        r"#if\s+0\b": ("Code disabled with #if 0", "Remove dead code instead of disabling it", Severity.LOW),
    }

    def review_file(self, filepath: str, content: str, lines: list[str]) -> list[Finding]:
        findings = []

        # --- Pattern-based checks ---
        for pattern, (name, suggestion, severity) in self.CODE_SMELLS.items():
            for match in re.finditer(pattern, content, re.IGNORECASE):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=severity,
                    category="Code Smell", agent=self.NAME,
                    description=name, suggestion=suggestion,
                ))

        for pattern, (name, suggestion, severity) in self.DEBUG_PATTERNS.items():
            for match in re.finditer(pattern, content, re.IGNORECASE):
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=severity,
                    category="Debug Code", agent=self.NAME,
                    description=name, suggestion=suggestion,
                ))

        # --- Long lines ---
        for i, line in enumerate(lines, 1):
            if len(line.rstrip()) > self.MAX_LINE_LENGTH:
                findings.append(Finding(
                    file=filepath, line=i, severity=Severity.LOW,
                    category="Formatting", agent=self.NAME,
                    description=f"Line exceeds {self.MAX_LINE_LENGTH} characters ({len(line.rstrip())} chars)",
                    suggestion="Break long lines for readability",
                ))

        # --- Function length analysis ---
        findings.extend(self._check_function_lengths(filepath, content, lines))

        # --- Nesting depth analysis ---
        findings.extend(self._check_nesting_depth(filepath, lines))

        # --- Magic numbers ---
        findings.extend(self._check_magic_numbers(filepath, content, lines))

        # --- Include guard check for headers ---
        if filepath.endswith(".h"):
            findings.extend(self._check_include_guard(filepath, content))

        # --- Commented-out code blocks ---
        findings.extend(self._check_commented_code(filepath, lines))

        # --- Function parameter count ---
        findings.extend(self._check_param_count(filepath, content))

        return findings

    def _check_function_lengths(self, filepath: str, content: str, lines: list[str]) -> list[Finding]:
        findings = []
        # Match C/C++ function definitions (simplified heuristic)
        func_pattern = re.compile(
            r"^(?:static\s+|inline\s+|NTSTATUS\s+|VOID\s+|void\s+|int\s+|bool\s+|BOOL\s+|auto\s+|"
            r"ULONG\s+|PVOID\s+|HANDLE\s+|float\s+|double\s+|DWORD\s+|SIZE_T\s+|PMDL\s+|"
            r"Vector3\s+|D3DMATRIX\s+|template\s*<[^>]*>\s*\w+\s+)*"
            r"(\w+)\s*\([^)]*\)\s*\{?\s*$",
            re.MULTILINE,
        )
        for match in func_pattern.finditer(content):
            func_name = match.group(1)
            start_line = content[:match.start()].count("\n") + 1
            # Count lines until matching closing brace
            brace_depth = 0
            func_lines = 0
            started = False
            for i, line in enumerate(lines[start_line - 1:], start_line):
                for ch in line:
                    if ch == "{":
                        brace_depth += 1
                        started = True
                    elif ch == "}":
                        brace_depth -= 1
                if started:
                    func_lines += 1
                if started and brace_depth == 0:
                    break

            if func_lines > self.MAX_FUNCTION_LINES:
                findings.append(Finding(
                    file=filepath, line=start_line, severity=Severity.MEDIUM,
                    category="Complexity", agent=self.NAME,
                    description=f"Function '{func_name}' is {func_lines} lines (limit: {self.MAX_FUNCTION_LINES})",
                    suggestion="Break large functions into smaller, focused helpers",
                ))
        return findings

    def _check_nesting_depth(self, filepath: str, lines: list[str]) -> list[Finding]:
        findings = []
        max_depth = 0
        max_depth_line = 0
        depth = 0
        for i, line in enumerate(lines, 1):
            depth += line.count("{") - line.count("}")
            if depth > max_depth:
                max_depth = depth
                max_depth_line = i

        if max_depth > self.MAX_NESTING_DEPTH:
            findings.append(Finding(
                file=filepath, line=max_depth_line, severity=Severity.MEDIUM,
                category="Complexity", agent=self.NAME,
                description=f"Maximum nesting depth is {max_depth} (limit: {self.MAX_NESTING_DEPTH})",
                suggestion="Reduce nesting with early returns, guard clauses, or extraction",
            ))
        return findings

    def _check_magic_numbers(self, filepath: str, content: str, lines: list[str]) -> list[Finding]:
        findings = []
        # Skip offset files — magic numbers are expected there
        if "offset" in filepath.lower():
            return findings

        magic_pattern = re.compile(r"(?<!=\s)(?<!\w)0x[0-9A-Fa-f]{4,}(?!\w)")
        reported_lines = set()
        for i, line in enumerate(lines, 1):
            stripped = line.strip()
            # Skip defines, constants, and comments
            if stripped.startswith("#define") or stripped.startswith("//") or "const" in stripped:
                continue
            matches = magic_pattern.findall(stripped)
            if matches and i not in reported_lines:
                reported_lines.add(i)
                findings.append(Finding(
                    file=filepath, line=i, severity=Severity.LOW,
                    category="Magic Number", agent=self.NAME,
                    description=f"Magic hex value(s) {', '.join(matches[:3])} used inline",
                    suggestion="Define named constants for clarity and maintainability",
                ))
        return findings

    def _check_include_guard(self, filepath: str, content: str) -> list[Finding]:
        findings = []
        has_pragma_once = "#pragma once" in content
        has_ifndef = re.search(r"#ifndef\s+\w+_H", content, re.IGNORECASE)
        if not has_pragma_once and not has_ifndef:
            findings.append(Finding(
                file=filepath, line=1, severity=Severity.MEDIUM,
                category="Header Guard", agent=self.NAME,
                description="Header file missing include guard (#pragma once or #ifndef)",
                suggestion="Add #pragma once or traditional include guards",
            ))
        return findings

    def _check_commented_code(self, filepath: str, lines: list[str]) -> list[Finding]:
        findings = []
        consecutive_comments = 0
        block_start = 0
        for i, line in enumerate(lines, 1):
            stripped = line.strip()
            if stripped.startswith("//") and (
                re.search(r"//\s*(if|for|while|return|int|void|auto|#)", stripped)
                or stripped.endswith(";")
                or stripped.endswith("{")
            ):
                if consecutive_comments == 0:
                    block_start = i
                consecutive_comments += 1
            else:
                if consecutive_comments >= 3:
                    findings.append(Finding(
                        file=filepath, line=block_start, severity=Severity.LOW,
                        category="Dead Code", agent=self.NAME,
                        description=f"Block of {consecutive_comments} commented-out code lines",
                        suggestion="Remove commented-out code — use version control to recover old code",
                    ))
                consecutive_comments = 0
        return findings

    def _check_param_count(self, filepath: str, content: str) -> list[Finding]:
        findings = []
        func_pattern = re.compile(
            r"(\w+)\s*\(([^)]{40,})\)\s*\{",
            re.MULTILINE,
        )
        for match in func_pattern.finditer(content):
            func_name = match.group(1)
            params = match.group(2)
            param_count = params.count(",") + 1
            if param_count > self.MAX_PARAMS:
                line_num = content[:match.start()].count("\n") + 1
                findings.append(Finding(
                    file=filepath, line=line_num, severity=Severity.LOW,
                    category="Complexity", agent=self.NAME,
                    description=f"Function '{func_name}' has {param_count} parameters (limit: {self.MAX_PARAMS})",
                    suggestion="Group related parameters into a struct or reduce parameter count",
                ))
        return findings

    def compute_verdict(self, review: AgentReview) -> Verdict:
        if review.critical_count > 0:
            return Verdict.FAIL
        if review.high_count > 0 or len(review.findings) >= 10:
            return Verdict.WARN
        return Verdict.PASS
