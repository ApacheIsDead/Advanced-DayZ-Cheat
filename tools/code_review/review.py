#!/usr/bin/env python3
"""
Multi-Agent Code Review System
===============================
Runs 3 specialized agents (Security, Quality, Performance) that independently
review the codebase, then feeds their findings through a consensus engine
that requires agreement before including items in the final report.

Usage:
    python -m tools.code_review.review [--path PATH] [--files FILE ...] [--json]
"""

import argparse
import json
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# Allow running as module or standalone
try:
    from .agents import SecurityAgent, QualityAgent, PerformanceAgent
    from .consensus import ConsensusEngine
    from .models import AgentReview, ConsensusResult
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from code_review.agents import SecurityAgent, QualityAgent, PerformanceAgent
    from code_review.consensus import ConsensusEngine
    from code_review.models import AgentReview, ConsensusResult


class MultiAgentReview:
    """Orchestrator that runs all 3 agents and produces consensus."""

    def __init__(self, root_dir: str):
        self.root_dir = root_dir
        self.agents = [
            SecurityAgent(root_dir),
            QualityAgent(root_dir),
            PerformanceAgent(root_dir),
        ]
        self.consensus_engine = ConsensusEngine()

    def run(self, files: list[str] | None = None) -> ConsensusResult:
        """Run all agents in parallel, then compute consensus."""
        print(f"\n[*] Starting multi-agent code review on: {self.root_dir}")
        print(f"[*] Agents: {', '.join(a.NAME for a in self.agents)}")
        print()

        reviews: list[AgentReview] = []
        start = time.time()

        # Run agents in parallel using threads
        with ThreadPoolExecutor(max_workers=3) as executor:
            futures = {
                executor.submit(agent.run, files): agent
                for agent in self.agents
            }
            for future in as_completed(futures):
                agent = futures[future]
                try:
                    review = future.result()
                    reviews.append(review)
                    icon = {"PASS": "+", "WARN": "!", "FAIL": "X"}[review.verdict.value]
                    print(f"  [{icon}] {review.agent_name} finished — "
                          f"{len(review.findings)} findings, verdict: {review.verdict.value}")
                except Exception as e:
                    print(f"  [E] {agent.NAME} failed: {e}", file=sys.stderr)

        elapsed = time.time() - start
        print(f"\n[*] All agents completed in {elapsed:.2f}s")
        print("[*] Computing consensus...\n")

        result = self.consensus_engine.evaluate(reviews)
        return result

    @staticmethod
    def result_to_dict(result: ConsensusResult) -> dict:
        """Serialize result to JSON-friendly dict."""
        return {
            "final_verdict": result.final_verdict.value,
            "consensus_score": result.consensus_score,
            "agreed_findings": [
                {
                    "file": f.file,
                    "line": f.line,
                    "severity": f.severity.value,
                    "category": f.category,
                    "description": f.description,
                    "suggestion": f.suggestion,
                    "agent": f.agent,
                }
                for f in result.agreed_findings
            ],
            "disputed_findings_count": len(result.disputed_findings),
            "agent_reviews": [
                {
                    "agent": r.agent_name,
                    "verdict": r.verdict.value,
                    "score": r.score,
                    "findings_count": len(r.findings),
                }
                for r in result.agent_reviews
            ],
        }


def main():
    parser = argparse.ArgumentParser(description="Multi-Agent Code Review System")
    parser.add_argument("--path", default=".", help="Root directory to review")
    parser.add_argument("--files", nargs="*", help="Specific files to review")
    parser.add_argument("--json", action="store_true", help="Output as JSON")
    args = parser.parse_args()

    root = str(Path(args.path).resolve())
    reviewer = MultiAgentReview(root)
    result = reviewer.run(args.files)

    if args.json:
        print(json.dumps(reviewer.result_to_dict(result), indent=2))
    else:
        print(result.summary)

    # Exit code based on verdict
    sys.exit({"PASS": 0, "WARN": 0, "FAIL": 1}[result.final_verdict.value])


if __name__ == "__main__":
    main()
