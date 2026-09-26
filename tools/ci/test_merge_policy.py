"""Seeded merge-history violations for the WP-0.1 post-merge gate."""

import os
import io
import subprocess
import tempfile
import unittest
import urllib.error
from dataclasses import replace
from pathlib import Path
from unittest.mock import call, patch

import merge_policy


class MergePolicyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.checkout = Path(self.temporary.name)
        # A developer's signing hooks or GIT_* overrides must not affect fixtures.
        self.git_env = {key: value for key, value in os.environ.items()
                        if not key.startswith("GIT_")}
        self.git_env.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        environment = patch.dict(os.environ, self.git_env, clear=True)
        environment.start()
        self.addCleanup(environment.stop)
        self.run_git("init", "-q", "-b", "main")
        self.run_git("config", "user.name", "Test Author")
        self.run_git("config", "user.email", "author@example.test")
        self.before = self.commit("base", "base")

    def run_git(self, *args, committer=None):
        env = self.git_env.copy()
        if committer:
            env["GIT_COMMITTER_NAME"], env["GIT_COMMITTER_EMAIL"] = committer
        result = subprocess.run(["git", "-C", str(self.checkout), *args], env=env,
                                check=True, capture_output=True, text=True)
        return result.stdout.strip()

    def commit(self, content, message):
        (self.checkout / "history.txt").write_text(content, encoding="utf-8")
        self.run_git("add", "history.txt")
        self.run_git("commit", "-q", "-m", message)
        return self.run_git("rev-parse", "HEAD")

    def landed(self, after):
        return merge_policy.landed_commits(self.before, after, self.checkout)

    def event(self, after):
        return {"ref": "refs/heads/main", "before": self.before, "after": after}

    def raw_commit(self, sha):
        tree, name, email, message = self.run_git(
            "show", "-s", "--format=%T%x00%an%x00%ae%x00%B", sha
        ).split("\0", 3)
        return {"sha": sha, "commit": {"tree": {"sha": tree},
                                        "author": {"name": name, "email": email},
                                        "message": message}}

    def pr(self, after, originals, branch="wp/0.1", head_repo="owner/repo"):
        return {"number": 7, "merged_at": "2026-01-01T00:00:00Z",
                "merge_commit_sha": after, "base": {"ref": "main"},
                "head": {"ref": branch, "sha": originals[-1],
                         "repo": {"full_name": head_repo}}, "commits": len(originals)}

    def audit_with(self, after, originals, pr=None, raw=None):
        pr = pr or self.pr(after, originals)
        raw = raw if raw is not None else [self.raw_commit(sha) for sha in originals]
        with patch.object(merge_policy, "api_get", side_effect=[[pr], pr, raw]) as get:
            result = merge_policy.audit(self.event(after), "owner/repo", self.checkout,
                                        "test-token")
        self.assertEqual(get.call_args_list, [
            call(f"repos/owner/repo/commits/{after}/pulls?per_page=100", "test-token"),
            call("repos/owner/repo/pulls/7", "test-token"),
            call("repos/owner/repo/pulls/7/commits?per_page=100&page=1", "test-token"),
        ])
        return result

    def collab_originals(self):
        self.run_git("checkout", "-q", "-b", "collab/s1")
        first = self.commit("first", "First\n\nHelios-Tx: 1..2\nHelios-Session: s1")
        second = self.commit("second", "Second\n\nHelios-Tx: 3..4\nHelios-Session: s1")
        self.run_git("checkout", "-q", "main")
        return [first, second]

    def collab_rebase(self, originals):
        for sha in originals:
            self.run_git("cherry-pick", sha,
                         committer=("GitHub", "noreply@github.com"))
        return self.run_git("rev-parse", "HEAD")

    def test_wp_squash_lands_one_commit(self):
        after = self.commit("finished", "WP-0.1: merge policy")
        self.assertIn("PR #7", self.audit_with(after, [after]))

    def test_multi_commit_wp_can_land_as_one_matching_tree(self):
        self.commit("first", "First WP commit")
        original_head = self.commit("finished", "Second WP commit")
        originals = self.run_git("rev-list", "--reverse", f"{self.before}..{original_head}").splitlines()
        self.run_git("reset", "--hard", self.before)
        squash_head = self.commit("finished", "Squashed WP")
        self.assertIn("1 landed commit", self.audit_with(squash_head, originals))

    def test_wp_rebase_lands_two_commits_and_fails(self):
        self.commit("first", "First WP commit")
        after = self.commit("finished", "Second WP commit")
        originals = self.run_git("rev-list", "--reverse", f"{self.before}..{after}").splitlines()
        with self.assertRaisesRegex(merge_policy.PolicyError, "landed as 2 commits"):
            self.audit_with(after, originals)

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

    def test_collab_rebase_preserves_authors_trees_and_trailers(self):
        originals = self.collab_originals()
        after = self.collab_rebase(originals)
        self.assertNotEqual(originals[-1], after)
        self.assertEqual(self.run_git("show", "-s", "--format=%cn", after), "GitHub")
        self.assertEqual(self.landed(after)[-1].author_name, "Test Author")
        self.assertIn("PR #7", self.audit_with(
            after, originals, self.pr(after, originals, branch="collab/s1")))

    def test_collab_squash_loses_commit_boundary(self):
        originals = self.collab_originals()
        after = self.commit("second", "Squashed publish\n\nHelios-Tx: 1..4")
        pr = self.pr(after, originals, branch="collab/s1")
        with self.assertRaisesRegex(merge_policy.PolicyError, "2 commits but 1 landed"):
            self.audit_with(after, originals, pr)
        self.assertEqual(len(self.landed(after)), 1)

    def test_collab_api_metadata_changes_fail(self):
        originals = self.collab_originals()
        after = self.collab_rebase(originals)
        pr = self.pr(after, originals, branch="collab/s1")
        for field, value, reason in (("email", "other@example.test", "author"),
                                     ("name", "Other", "author"),
                                     ("message", "Second", "trailers")):
            with self.subTest(field=field):
                raw = [self.raw_commit(sha) for sha in originals]
                if field == "message":
                    raw[-1]["commit"][field] = value
                else:
                    raw[-1]["commit"]["author"][field] = value
                with self.assertRaisesRegex(merge_policy.PolicyError, f"changed {reason}"):
                    self.audit_with(after, originals, pr, raw)

    def test_collab_changed_tree_fails(self):
        originals = self.collab_originals()
        after = self.collab_rebase(originals)
        pr = self.pr(after, originals, branch="collab/s1")
        raw = [self.raw_commit(sha) for sha in originals]
        raw[-1]["commit"]["tree"]["sha"] = "0" * 40
        with self.assertRaisesRegex(merge_policy.PolicyError, "changed tree"):
            self.audit_with(after, originals, pr, raw)

    def test_fork_collab_branch_is_wp_class(self):
        originals = self.collab_originals()
        after = self.collab_rebase(originals)
        pr = self.pr(after, originals, branch="collab/s1", head_repo="fork/repo")
        with self.assertRaisesRegex(merge_policy.PolicyError, "WP-class PR landed as 2"):
            self.audit_with(after, originals, pr)

    def test_similar_prefix_is_wp_class(self):
        self.commit("first", "First")
        later = self.commit("second", "Second")
        expected = self.landed(later)
        with self.assertRaisesRegex(merge_policy.PolicyError, "WP-class PR landed as 2"):
            merge_policy.check_policy("collaborate/x", expected, expected)

    def test_unrelated_push_is_rejected(self):
        after = self.commit("next", "Next")
        self.run_git("checkout", "-q", "--orphan", "unrelated")
        self.commit("other", "Other")
        with self.assertRaisesRegex(merge_policy.PolicyError, "not a descendant"):
            merge_policy.landed_commits(after, self.run_git("rev-parse", "HEAD"),
                                        self.checkout)

    def test_audit_rejects_direct_push_without_associated_pr(self):
        after = self.commit("next", "Next")
        with patch.object(merge_policy, "api_get", return_value=[]) as get, \
                patch.object(merge_policy.time, "sleep") as sleep:
            with self.assertRaisesRegex(merge_policy.PolicyError, "expected one merged PR"):
                merge_policy.audit(self.event(after), "owner/repo", self.checkout, "test-token")
        self.assertEqual(get.call_count, 3)
        self.assertEqual(sleep.call_args_list, [call(1), call(2)])

    def test_commit_association_lag_is_retried(self):
        after = self.commit("next", "Next")
        pr = self.pr(after, [after])
        with patch.object(merge_policy, "api_get",
                          side_effect=[[], [pr], pr, [self.raw_commit(after)]]) as get, \
                patch.object(merge_policy.time, "sleep") as sleep:
            self.assertIn("PR #7", merge_policy.audit(self.event(after), "owner/repo",
                                                      self.checkout, "test-token"))
        self.assertEqual(get.call_count, 4)
        sleep.assert_called_once_with(1)

    def test_transient_api_error_is_retried(self):
        error = urllib.error.HTTPError("https://api.github.test", 502, "bad gateway", {}, None)
        with patch.object(merge_policy.urllib.request, "urlopen",
                          side_effect=[error, io.BytesIO(b'{"ok": true}')]) as open_url, \
                patch.object(merge_policy.time, "sleep") as sleep:
            self.assertEqual(merge_policy.api_get("repos/owner/repo/pulls/7", "test-token"),
                             {"ok": True})
        self.assertEqual(open_url.call_count, 2)
        sleep.assert_called_once_with(1)

    def test_ambiguous_associated_prs_fail(self):
        after = self.commit("next", "Next")
        pr = self.pr(after, [after])
        with patch.object(merge_policy, "api_get", return_value=[pr, pr]) as get:
            with self.assertRaisesRegex(merge_policy.PolicyError, "found 2"):
                merge_policy.audit(self.event(after), "owner/repo", self.checkout,
                                   "test-token")
        get.assert_called_once()

    def test_pr_detail_must_target_main_and_match_merge_sha(self):
        after = self.commit("next", "Next")
        associated = self.pr(after, [after])
        for change, reason in (({"base": {"ref": "release/1"}}, "did not target main"),
                               ({"merge_commit_sha": "0" * 40}, "merge SHA does not match")):
            with self.subTest(change=change):
                detail = {**associated, **change}
                with patch.object(merge_policy, "api_get", side_effect=[[associated], detail]) as get:
                    with self.assertRaisesRegex(merge_policy.PolicyError, reason):
                        merge_policy.audit(self.event(after), "owner/repo", self.checkout,
                                           "test-token")
                self.assertEqual(get.call_count, 2)

    def test_pr_commit_list_must_end_at_head_sha(self):
        after = self.commit("next", "Next")
        pr = self.pr(after, [after])
        pr["head"]["sha"] = "0" * 40
        with self.assertRaisesRegex(merge_policy.PolicyError, "does not end"):
            self.audit_with(after, [after], pr)

    def test_pr_commit_pagination_and_count(self):
        commits = [{"sha": f"{number:040x}"} for number in range(101)]
        with patch.object(merge_policy, "api_get", side_effect=[commits[:100], commits[100:]]) as get:
            self.assertEqual(merge_policy.all_pr_commits("owner/repo", 7, 101, "test-token"), commits)
        self.assertEqual(get.call_args_list, [
            call("repos/owner/repo/pulls/7/commits?per_page=100&page=1", "test-token"),
            call("repos/owner/repo/pulls/7/commits?per_page=100&page=2", "test-token"),
        ])
        with patch.object(merge_policy, "api_get", side_effect=[commits[:100], []]):
            with self.assertRaisesRegex(merge_policy.PolicyError, "reports 102 commits"):
                merge_policy.all_pr_commits("owner/repo", 7, 102, "test-token")
        with patch.object(merge_policy, "api_get") as get:
            with self.assertRaisesRegex(merge_policy.PolicyError, "caps this at 250"):
                merge_policy.all_pr_commits("owner/repo", 7, 251, "test-token")
        get.assert_not_called()

    def test_audit_event_and_count_guards(self):
        after = self.commit("next", "Next")
        event = self.event(after)
        for changed, repository, token, reason in (
            ({"ref": "refs/heads/other"}, "owner/repo", "test-token", "non-deleted main"),
            ({"deleted": True}, "owner/repo", "test-token", "non-deleted main"),
            ({}, "not-a-repository", "test-token", "owner/repo"),
            ({"before": "0"}, "owner/repo", "test-token", "invalid before"),
            ({}, "owner/repo", "", "GITHUB_TOKEN"),
        ):
            with self.subTest(changed=changed, repository=repository, token=token):
                with patch.object(merge_policy, "api_get") as get:
                    with self.assertRaisesRegex(merge_policy.PolicyError, reason):
                        merge_policy.audit({**event, **changed}, repository, self.checkout, token)
                get.assert_not_called()
        pr = self.pr(after, [after])
        pr["commits"] = 0
        with patch.object(merge_policy, "api_get", side_effect=[[pr], pr]) as get:
            with self.assertRaisesRegex(merge_policy.PolicyError, "invalid commit count"):
                merge_policy.audit(event, "owner/repo", self.checkout, "test-token")
        self.assertEqual(get.call_count, 2)


if __name__ == "__main__":
    unittest.main()
