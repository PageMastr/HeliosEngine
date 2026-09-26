"""Seeded merge-history violations for the WP-0.1 post-merge gate."""

import subprocess
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

import merge_policy


class MergePolicyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.checkout = Path(self.temporary.name)
        self.run_git("init", "-q", "-b", "main")
        self.run_git("config", "user.name", "Test Author")
        self.run_git("config", "user.email", "author@example.test")
        self.before = self.commit("base", "base")

    def run_git(self, *args):
        result = subprocess.run(["git", "-C", str(self.checkout), *args],
                                check=True, capture_output=True, text=True)
        return result.stdout.strip()

    def commit(self, content, message):
        (self.checkout / "history.txt").write_text(content, encoding="utf-8")
        self.run_git("add", "history.txt")
        self.run_git("commit", "-q", "-m", message)
        return self.run_git("rev-parse", "HEAD")

    def landed(self, after):
        return merge_policy.landed_commits(self.before, after, self.checkout)

    def test_wp_squash_lands_one_commit(self):
        after = self.commit("finished", "WP-0.1: merge policy")
        landed = self.landed(after)
        merge_policy.check_policy("agent/codex/wp-0.1", landed, landed)
        self.assertEqual(len(landed), 1)

    def test_multi_commit_wp_can_land_as_one_matching_tree(self):
        self.commit("first", "First WP commit")
        original_head = self.commit("finished", "Second WP commit")
        expected = self.landed(original_head)
        self.run_git("reset", "--hard", self.before)
        squash_head = self.commit("finished", "Squashed WP")
        landed = self.landed(squash_head)
        merge_policy.check_policy("wp/0.1", expected, landed)
        self.assertEqual(len(expected), 2)
        self.assertEqual(len(landed), 1)

    def test_wp_rebase_lands_two_commits_and_fails(self):
        self.commit("first", "First WP commit")
        after = self.commit("finished", "Second WP commit")
        expected = self.landed(after)
        with self.assertRaisesRegex(merge_policy.PolicyError, "landed as 2 commits"):
            merge_policy.check_policy("wp/0.1", expected, self.landed(after))

    def test_merge_commit_is_not_linear_history(self):
        self.run_git("checkout", "-q", "-b", "feature")
        self.commit("finished", "WP commit")
        self.run_git("checkout", "-q", "main")
        self.run_git("merge", "--no-ff", "-q", "feature", "-m", "Merge feature")
        with self.assertRaisesRegex(merge_policy.PolicyError, "merge commit or nonlinear"):
            self.landed(self.run_git("rev-parse", "HEAD"))

    def test_wp_squash_with_changed_tree_fails(self):
        after = self.commit("finished", "WP-0.1: merge policy")
        landed = self.landed(after)
        expected = [replace(landed[0], tree="0" * 40)]
        with self.assertRaisesRegex(merge_policy.PolicyError, "differs from the reviewed"):
            merge_policy.check_policy("wp/0.1", expected, landed)

    def test_collab_rebase_keeps_each_commit_and_trailers(self):
        self.commit("first", "First\n\nHelios-Tx: 1..2\nHelios-Session: s1")
        after = self.commit("second", "Second\n\nHelios-Tx: 3..4\nHelios-Session: s1")
        landed = self.landed(after)
        merge_policy.check_policy("collab/s1", landed, landed)
        self.assertEqual(landed[0].trailers, ("Helios-Tx: 1..2", "Helios-Session: s1"))

    def test_collab_squash_loses_commit_boundary(self):
        first = self.commit("first", "First\n\nHelios-Tx: 1..2")
        second = self.commit("second", "Second\n\nHelios-Tx: 3..4")
        expected = self.landed(second)
        self.run_git("reset", "--hard", self.before)
        self.commit("second", "Squashed publish\n\nHelios-Tx: 1..4")
        with self.assertRaisesRegex(merge_policy.PolicyError, "2 commits but 1 landed"):
            merge_policy.check_policy("collab/s1", expected,
                                      self.landed(self.run_git("rev-parse", "HEAD")))
        self.assertNotEqual(first, second)

    def test_collab_changes_to_author_tree_or_trailers_fail(self):
        after = self.commit("first", "First\n\nHelios-Tx: 1..2")
        original = self.landed(after)
        for replacement, reason in (({"author_name": "Other"}, "author"),
                                    ({"tree": "0" * 40}, "tree"),
                                    ({"trailers": ()}, "trailers")):
            with self.subTest(reason=reason):
                changed = [replace(original[0], **replacement)]
                with self.assertRaisesRegex(merge_policy.PolicyError, f"changed {reason}"):
                    merge_policy.check_policy("collab/s1", original, changed)

    def test_unrelated_push_is_rejected(self):
        after = self.commit("next", "Next")
        self.run_git("checkout", "-q", "--orphan", "unrelated")
        self.commit("other", "Other")
        with self.assertRaises(merge_policy.PolicyError):
            merge_policy.landed_commits(after, self.run_git("rev-parse", "HEAD"),
                                        self.checkout)

    def test_audit_rejects_direct_push_without_associated_pr(self):
        after = self.commit("next", "Next")
        event = {"ref": "refs/heads/main", "before": self.before, "after": after}
        with patch.object(merge_policy, "api_get", return_value=[]):
            with self.assertRaisesRegex(merge_policy.PolicyError, "expected one merged PR"):
                merge_policy.audit(event, "owner/repo", self.checkout, "test-token")

    def test_audit_accepts_wp_squash_with_matching_pr(self):
        after = self.commit("next", "Next")
        event = {"ref": "refs/heads/main", "before": self.before, "after": after}
        commit = self.landed(after)[0]
        pr = {"number": 7, "merged_at": "2026-01-01T00:00:00Z", "merge_commit_sha": after,
              "base": {"ref": "main"}, "head": {"ref": "wp/0.1"}, "commits": 1}
        raw_commit = {"commit": {"author": {"name": commit.author_name,
                                               "email": commit.author_email},
                                 "tree": {"sha": commit.tree}, "message": "Next"}}
        with patch.object(merge_policy, "api_get", side_effect=[[pr], pr, [raw_commit]]):
            self.assertIn("PR #7", merge_policy.audit(event, "owner/repo", self.checkout,
                                                       "test-token"))


if __name__ == "__main__":
    unittest.main()
