"""Consensus Engine - Requires agents to agree on findings before final verdict."""

from collections import defaultdict
from .models import Finding, AgentReview, ConsensusResult, Verdict, Severity


class ConsensusEngine:
    """
    Merges findings from multiple agents and determines consensus.

    Rules:
    - A finding is "agreed" if 2+ agents flagged the same file within a proximity window.
    - CRITICAL findings from any single agent are always included (escalated).
    - Final verdict requires majority (2/3) agreement.
    - Consensus score is the average of all agent scores, weighted by agreement.
    """

    PROXIMITY_LINES = 10  # Two findings within N lines of each other in the same file are "related"

    def evaluate(self, reviews: list[AgentReview]) -> ConsensusResult:
        all_findings: list[Finding] = []
        for r in reviews:
            all_findings.extend(r.findings)

        agreed, disputed = self._classify_findings(all_findings, len(reviews))
        final_verdict = self._vote_verdict(reviews)
        consensus_score = self._compute_consensus_score(reviews, len(agreed), len(disputed))
        summary = self._build_summary(reviews, agreed, disputed, final_verdict, consensus_score)

        return ConsensusResult(
            agreed_findings=agreed,
            disputed_findings=disputed,
            agent_reviews=reviews,
            final_verdict=final_verdict,
            consensus_score=consensus_score,
            summary=summary,
        )

    def _classify_findings(
        self, findings: list[Finding], num_agents: int
    ) -> tuple[list[Finding], list[Finding]]:
        """Separate findings into agreed (multi-agent) and disputed (single-agent)."""
        # Group findings by file
        by_file: dict[str, list[Finding]] = defaultdict(list)
        for f in findings:
            by_file[f.file].append(f)

        agreed: list[Finding] = []
        disputed: list[Finding] = []

        for file, file_findings in by_file.items():
            # Sort by line number
            file_findings.sort(key=lambda f: f.line)

            for finding in file_findings:
                # CRITICAL findings from any agent are always agreed upon
                if finding.severity == Severity.CRITICAL:
                    agreed.append(finding)
                    continue

                # Check if another agent flagged something nearby
                has_corroboration = any(
                    other.agent != finding.agent
                    and abs(other.line - finding.line) <= self.PROXIMITY_LINES
                    for other in file_findings
                )

                if has_corroboration:
                    agreed.append(finding)
                else:
                    disputed.append(finding)

        # Deduplicate agreed findings (keep highest severity per location)
        agreed = self._deduplicate(agreed)
        return agreed, disputed

    def _deduplicate(self, findings: list[Finding]) -> list[Finding]:
        """Remove near-duplicate findings, keeping the highest severity."""
        seen: dict[str, Finding] = {}
        for f in findings:
            key = f"{f.file}:{f.line // 5}:{f.category}"
            existing = seen.get(key)
            if existing is None or f.severity.weight > existing.severity.weight:
                seen[key] = f
        return sorted(seen.values(), key=lambda f: (-f.severity.weight, f.file, f.line))

    def _vote_verdict(self, reviews: list[AgentReview]) -> Verdict:
        """Majority vote on final verdict."""
        votes = {"FAIL": 0, "WARN": 0, "PASS": 0}
        for r in reviews:
            votes[r.verdict.value] += 1

        # Unanimous or majority FAIL
        if votes["FAIL"] >= 2:
            return Verdict.FAIL
        # Any FAIL + any WARN = WARN
        if votes["FAIL"] >= 1:
            return Verdict.WARN
        # Majority WARN
        if votes["WARN"] >= 2:
            return Verdict.WARN
        return Verdict.PASS

    def _compute_consensus_score(
        self, reviews: list[AgentReview], agreed_count: int, disputed_count: int
    ) -> float:
        """Score from 0-100 representing overall code health consensus."""
        avg_score = sum(r.score for r in reviews) / len(reviews)
        total = agreed_count + disputed_count
        agreement_ratio = agreed_count / total if total > 0 else 1.0
        # Weight: 70% agent scores, 30% agreement strength
        return round(avg_score * 0.7 + agreement_ratio * 30, 1)

    def _build_summary(
        self,
        reviews: list[AgentReview],
        agreed: list[Finding],
        disputed: list[Finding],
        verdict: Verdict,
        score: float,
    ) -> str:
        lines = []
        lines.append("=" * 72)
        lines.append("  MULTI-AGENT CODE REVIEW — CONSENSUS REPORT")
        lines.append("=" * 72)
        lines.append("")

        # Agent summaries
        lines.append("AGENT VERDICTS:")
        lines.append("-" * 40)
        for r in reviews:
            icon = {"PASS": "[OK]", "WARN": "[!!]", "FAIL": "[XX]"}[r.verdict.value]
            lines.append(f"  {icon} {r.agent_name:<20} {r.verdict.value:<6} (score: {r.score:.1f}/100)")
            lines.append(f"      Focus: {r.focus}")
            lines.append(f"      Findings: {len(r.findings)}")
        lines.append("")

        # Consensus verdict
        verdict_str = {
            "PASS": "PASS — All agents agree the code meets standards",
            "WARN": "WARN — Agents identified issues requiring attention",
            "FAIL": "FAIL — Critical issues found, agents recommend fixes before merge",
        }[verdict.value]
        lines.append(f"FINAL CONSENSUS: {verdict_str}")
        lines.append(f"CONSENSUS SCORE: {score}/100")
        lines.append("")

        # Agreed findings (by severity)
        if agreed:
            lines.append(f"AGREED FINDINGS ({len(agreed)} — corroborated by multiple agents):")
            lines.append("-" * 60)
            for f in agreed:
                lines.append(f"  [{f.severity.value:<8}] {f.file}:{f.line}")
                lines.append(f"            {f.description}")
                if f.suggestion:
                    lines.append(f"            -> {f.suggestion}")
                lines.append(f"            (Reported by: {f.agent})")
                lines.append("")

        # Disputed findings (condensed)
        if disputed:
            lines.append(f"DISPUTED FINDINGS ({len(disputed)} — flagged by single agent only):")
            lines.append("-" * 60)
            by_agent: dict[str, list[Finding]] = defaultdict(list)
            for f in disputed:
                by_agent[f.agent].append(f)
            for agent, agent_findings in by_agent.items():
                lines.append(f"  {agent}:")
                for f in agent_findings[:10]:  # Cap display at 10 per agent
                    lines.append(f"    [{f.severity.value:<8}] {f.file}:{f.line} — {f.description}")
                if len(agent_findings) > 10:
                    lines.append(f"    ... and {len(agent_findings) - 10} more")
                lines.append("")

        lines.append("=" * 72)
        lines.append("  Review complete. Agents must agree — above are the consensus results.")
        lines.append("=" * 72)
        return "\n".join(lines)
