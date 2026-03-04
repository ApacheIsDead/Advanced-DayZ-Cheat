"""Data models for the multi-agent code review system."""

from dataclasses import dataclass, field
from enum import Enum


class Severity(Enum):
    CRITICAL = "CRITICAL"
    HIGH = "HIGH"
    MEDIUM = "MEDIUM"
    LOW = "LOW"
    INFO = "INFO"

    @property
    def weight(self) -> int:
        return {
            Severity.CRITICAL: 5,
            Severity.HIGH: 4,
            Severity.MEDIUM: 3,
            Severity.LOW: 2,
            Severity.INFO: 1,
        }[self]


class Verdict(Enum):
    PASS = "PASS"
    WARN = "WARN"
    FAIL = "FAIL"


@dataclass
class Finding:
    file: str
    line: int
    severity: Severity
    category: str
    description: str
    agent: str
    suggestion: str = ""

    @property
    def location_key(self) -> str:
        return f"{self.file}:{self.line}"


@dataclass
class AgentReview:
    agent_name: str
    focus: str
    findings: list[Finding] = field(default_factory=list)
    verdict: Verdict = Verdict.PASS
    summary: str = ""

    @property
    def critical_count(self) -> int:
        return sum(1 for f in self.findings if f.severity == Severity.CRITICAL)

    @property
    def high_count(self) -> int:
        return sum(1 for f in self.findings if f.severity == Severity.HIGH)

    @property
    def score(self) -> float:
        if not self.findings:
            return 100.0
        penalty = sum(f.severity.weight * 2.5 for f in self.findings)
        return max(0.0, 100.0 - penalty)


@dataclass
class ConsensusResult:
    agreed_findings: list[Finding]
    disputed_findings: list[Finding]
    agent_reviews: list[AgentReview]
    final_verdict: Verdict
    consensus_score: float
    summary: str = ""
