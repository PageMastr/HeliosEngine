"""Check that a push to main preserved its pull request's merge policy (WP-0.1)."""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path


SHA = re.compile(r"[0-9a-f]{40}\Z")
REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z")


class PolicyError(Exception):
    """A push cannot be verified or violates the branch-class merge policy."""


class VerificationError(PolicyError):
    """External data was unavailable; investigate before treating this as a violation."""


@dataclass(frozen=True)
class Commit:
    tree: str
    author_name: str
    author_email: str
    trailers: tuple[str, ...]


def git(*args: str, checkout: Path, input_text: str | None = None) -> str:
    command = ["git", "-C", str(checkout), *args]
    completed = subprocess.run(command, input=input_text, text=True, capture_output=True, check=False)
    if completed.returncode:
        raise PolicyError(f"git {args[0]} failed: {completed.stderr.strip()}")
    return completed.stdout


def trailers(message: str, checkout: Path) -> tuple[str, ...]:
    # Git's own parser handles continuation lines and trailer separators consistently.
    return tuple(git("interpret-trailers", "--parse", checkout=checkout, input_text=message).splitlines())


def landed_commits(before: str, after: str, checkout: Path) -> list[Commit]:
    if not SHA.fullmatch(before) or not SHA.fullmatch(after):
        raise PolicyError("push event has an invalid before or after SHA")
    try:
        git("merge-base", "--is-ancestor", before, after, checkout=checkout)
    except PolicyError as error:
        raise PolicyError("push is not a descendant of the previous main head") from error
    hashes = git("rev-list", "--reverse", f"{before}..{after}", checkout=checkout).splitlines()
    if not hashes:
        raise PolicyError("push introduced no commits")
    commits = []
    parent = before
    for sha in hashes:
        ancestry = git("rev-list", "--parents", "-n", "1", sha, checkout=checkout).split()
        if ancestry != [sha, parent]:
            raise PolicyError("push contains a merge commit or nonlinear history")
        fields = git("show", "-s", "--no-patch", "--format=%T%x00%an%x00%ae%x00%B", sha,
                     checkout=checkout).split("\0", 3)
        if len(fields) != 4:
            raise PolicyError(f"cannot read landed commit {sha}")
        tree, author_name, author_email, message = fields
        commits.append(Commit(tree, author_name, author_email, trailers(message, checkout)))
        parent = sha
    if parent != after:
        raise PolicyError("push head does not match the last landed commit")
    return commits


def pr_commits(data: list[dict], checkout: Path) -> list[Commit]:
    commits = []
    for item in data:
        try:
            raw = item["commit"]
            author = raw["author"]
            commits.append(Commit(raw["tree"]["sha"], author["name"], author["email"],
                                  trailers(raw["message"], checkout)))
        except (KeyError, TypeError) as error:
            raise VerificationError("GitHub returned incomplete PR commit metadata") from error
    if not commits:
        raise PolicyError("merged PR has no commits")
    return commits


def check_policy(branch: str, expected: list[Commit], landed: list[Commit],
                 *, collab: bool | None = None) -> None:
    if not expected or not landed:
        raise PolicyError("cannot compare empty PR or landed commit history")
    if collab is None:
        collab = branch.startswith("collab/")
    if collab:
        if len(landed) != len(expected):
            raise PolicyError(
                f"collab/* PR has {len(expected)} commits but {len(landed)} landed; "
                "publish commits must be rebase-merged, never squashed"
            )
        for number, (original, merged) in enumerate(zip(expected, landed), start=1):
            if (original.author_name, original.author_email) != (
                merged.author_name, merged.author_email
            ):
                raise PolicyError(f"collab/* commit {number} changed author")
            if original.tree != merged.tree:
                raise PolicyError(f"collab/* commit {number} changed tree")
            if original.trailers != merged.trailers:
                raise PolicyError(f"collab/* commit {number} changed trailers")
    else:
        if len(landed) != 1:
            raise PolicyError(
                f"WP-class PR landed as {len(landed)} commits; squash it to exactly one commit"
            )
        if landed[0].tree != expected[-1].tree:
            raise PolicyError(
                "WP-class squash tree differs from the reviewed PR head tree; "
                "update the branch on main and re-merge after integration verification"
            )


def api_get(path: str, token: str) -> object:
    base = os.environ.get("GITHUB_API_URL", "https://api.github.com").rstrip("/")
    request = urllib.request.Request(
        f"{base}/{path}",
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            # REST 2026-03-10 omits merge_commit_sha, which this audit needs.
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    for attempt in range(3):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            if error.code in (429, 500, 502, 503, 504) and attempt < 2:
                time.sleep(2 ** attempt)
                continue
            raise VerificationError(f"GitHub API {path}: HTTP {error.code}") from error
        except urllib.error.URLError as error:
            if attempt < 2:
                time.sleep(2 ** attempt)
                continue
            raise VerificationError(f"GitHub API {path}: {error}") from error
        except ValueError as error:
            raise VerificationError(f"GitHub API {path}: {error}") from error
    raise VerificationError(f"GitHub API {path} failed after retries")


def merged_pr(repository: str, after: str, token: str) -> dict:
    for attempt in range(3):
        associated = api_get(f"repos/{repository}/commits/{after}/pulls?per_page=100", token)
        if not isinstance(associated, list):
            raise VerificationError("GitHub did not return a PR list for the landed commit")
        matches = [pr for pr in associated if isinstance(pr, dict)
                   and pr.get("merged_at") and pr.get("merge_commit_sha") == after]
        if matches or attempt == 2:
            break
        # Commit-to-PR association can lag the push that triggered this job.
        time.sleep(2 ** attempt)
    if not matches:
        raise VerificationError(
            f"expected one merged PR associated with {after}, found 0 after retries; "
            "investigate a direct push or GitHub association lag"
        )
    if len(matches) != 1:
        raise PolicyError(
            f"expected one merged PR associated with {after}, found {len(matches)}; "
            "direct pushes and ambiguous merges are forbidden"
        )
    number = matches[0]["number"]
    detail = api_get(f"repos/{repository}/pulls/{number}", token)
    if not isinstance(detail, dict) or not isinstance(detail.get("base"), dict) \
            or detail["base"].get("ref") != "main":
        raise PolicyError(f"PR #{number} did not target main")
    if detail.get("merge_commit_sha") != after:
        raise PolicyError(f"PR #{number} merge SHA does not match pushed head")
    return detail


def all_pr_commits(repository: str, number: int, expected_count: int, token: str) -> list[dict]:
    if expected_count > 250:
        raise PolicyError(f"PR #{number} has {expected_count} commits; GitHub's PR API caps this at 250")
    found = []
    for page in range(1, 4):
        data = api_get(f"repos/{repository}/pulls/{number}/commits?per_page=100&page={page}", token)
        if not isinstance(data, list):
            raise VerificationError(f"GitHub did not return commits for PR #{number}")
        found.extend(data)
        if len(data) < 100:
            break
    if len(found) != expected_count:
        raise VerificationError(
            f"PR #{number} reports {expected_count} commits, but the API returned {len(found)}"
        )
    return found


def audit(event: dict, repository: str, checkout: Path, token: str) -> str:
    if event.get("ref") != "refs/heads/main" or event.get("deleted"):
        raise PolicyError("merge-policy runs only on a non-deleted main push")
    if not REPOSITORY.fullmatch(repository):
        raise PolicyError("GITHUB_REPOSITORY must be owner/repo")
    before, after = event.get("before", ""), event.get("after", "")
    if not SHA.fullmatch(before) or not SHA.fullmatch(after):
        raise PolicyError("push event has an invalid before or after SHA")
    if not token:
        raise PolicyError("GITHUB_TOKEN is required to inspect the merged PR")
    landed = landed_commits(before, after, checkout)
    pr = merged_pr(repository, after, token)
    number = pr["number"]
    head = pr.get("head")
    if not isinstance(head, dict):
        raise PolicyError(f"PR #{number} has no head branch")
    branch = head.get("ref", "")
    if not branch:
        raise PolicyError(f"PR #{number} has no head branch")
    head_sha = head.get("sha", "")
    if not isinstance(head_sha, str) or not SHA.fullmatch(head_sha):
        raise PolicyError(f"PR #{number} has an invalid head SHA")
    head_repository = head.get("repo")
    head_repository_name = (head_repository.get("full_name")
                            if isinstance(head_repository, dict) else None)
    expected_count = pr.get("commits")
    if not isinstance(expected_count, int) or expected_count < 1:
        raise PolicyError(f"PR #{number} has an invalid commit count")
    raw_commits = all_pr_commits(repository, number, expected_count, token)
    if not isinstance(raw_commits[-1], dict) or raw_commits[-1].get("sha") != head_sha:
        raise PolicyError(f"PR #{number} commit list does not end at its reviewed head SHA")
    expected = pr_commits(raw_commits, checkout)
    collab = branch.startswith("collab/") and head_repository_name == repository
    check_policy(branch, expected, landed, collab=collab)
    return f"PR #{number} ({branch}): {len(landed)} landed commit(s) obey merge policy"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", type=Path, required=True)
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""))
    parser.add_argument("--checkout", type=Path, default=Path("."))
    args = parser.parse_args()
    try:
        event = json.loads(args.event.read_text(encoding="utf-8"))
        print(audit(event, args.repository, args.checkout, os.environ.get("GITHUB_TOKEN", "")))
    except VerificationError as error:
        print(f"merge-policy: could not verify: {error}", file=sys.stderr)
        return 1
    except (OSError, ValueError, PolicyError) as error:
        print(f"merge-policy: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
