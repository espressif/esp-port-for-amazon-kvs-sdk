# Contributing

Thanks for your interest in improving this ESP-IDF port of the Amazon Kinesis
Video Streams WebRTC SDK. Bug reports, fixes, and documentation improvements are
all welcome.

## Reporting bugs / requesting features

Use the GitHub issue tracker. Before opening one, search existing open and
recently closed issues to avoid duplicates. A good report includes:

- a reproducible test case or steps to trigger it
- the commit/version you're on
- your target (`esp32p4`, `esp32s3`, …), ESP-IDF version, and board
- any local modifications relevant to the problem

## Pull requests

Before sending a PR:

1. Work against the latest `main`.
2. Check open and recently merged PRs so you're not duplicating effort.
3. For anything substantial, open an issue first to discuss the approach — we'd
   hate for your time to be wasted.

When you send it:

- Keep the change focused; avoid unrelated reformatting — it makes review harder.
- Build the examples you touched and make sure the unit tests (`test_app/`) pass.
- Use clear commit messages.
- Watch the CI results on the PR and stay in the conversation.

## Security issues

Please do **not** open a public issue for a suspected security vulnerability.
Report it privately via this repository's GitHub **Security Advisories**
("Report a vulnerability") so it can be addressed before disclosure.

## Licensing

This project is licensed under Apache-2.0 — see [LICENSE](LICENSE). By
contributing, you agree your contribution is licensed under the same terms.
