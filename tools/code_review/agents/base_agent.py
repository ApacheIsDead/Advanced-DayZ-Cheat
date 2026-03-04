"""Base agent class for the multi-agent code review system."""

from abc import ABC, abstractmethod
from pathlib import Path
from ..models import Finding, AgentReview, Verdict


class BaseAgent(ABC):
    NAME: str = "BaseAgent"
    FOCUS: str = ""
    FILE_EXTENSIONS = {".c", ".cpp", ".h", ".hpp", ".cc", ".cxx", ".hxx"}

    def __init__(self, root_dir: str):
        self.root_dir = Path(root_dir)

    def run(self, files: list[str] | None = None) -> AgentReview:
        """Run the agent review on the specified files or discover files automatically."""
        if files is None:
            files = self._discover_files()

        all_findings: list[Finding] = []
        for filepath in files:
            try:
                content = Path(filepath).read_text(errors="replace")
                lines = content.splitlines()
                file_findings = self.review_file(filepath, content, lines)
                all_findings.extend(file_findings)
            except (OSError, IOError) as e:
                all_findings.append(Finding(
                    file=filepath, line=0, severity=__import__("tools.code_review.models", fromlist=["Severity"]).Severity.INFO,
                    category="File Error", agent=self.NAME,
                    description=f"Could not read file: {e}",
                ))

        review = AgentReview(
            agent_name=self.NAME,
            focus=self.FOCUS,
            findings=all_findings,
        )
        review.verdict = self.compute_verdict(review)
        review.summary = self._generate_summary(review)
        return review

    @abstractmethod
    def review_file(self, filepath: str, content: str, lines: list[str]) -> list[Finding]:
        """Review a single file and return findings."""
        ...

    @abstractmethod
    def compute_verdict(self, review: AgentReview) -> Verdict:
        """Determine PASS/WARN/FAIL based on findings."""
        ...

    def _discover_files(self) -> list[str]:
        """Find all reviewable source files under root_dir."""
        files = []
        for ext in self.FILE_EXTENSIONS:
            files.extend(str(p) for p in self.root_dir.rglob(f"*{ext}")
                         if ".git" not in p.parts and "tools" not in p.parts)
        return sorted(files)

    def _generate_summary(self, review: AgentReview) -> str:
        sev_counts = {}
        for f in review.findings:
            sev_counts[f.severity.value] = sev_counts.get(f.severity.value, 0) + 1
        parts = [f"{review.agent_name} [{review.verdict.value}] — {len(review.findings)} finding(s)"]
        if sev_counts:
            breakdown = ", ".join(f"{k}: {v}" for k, v in sorted(sev_counts.items()))
            parts.append(f"  Breakdown: {breakdown}")
        return "\n".join(parts)
