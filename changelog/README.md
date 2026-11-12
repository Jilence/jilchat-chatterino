<!--
SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>

SPDX-License-Identifier: MIT
-->

# Changelog notes

A pull request can add a file here with what it changes for users, for example
`changelog/raid-shoutouts.md`. One line per change, starting with `New`,
`Fixed`, `Changed` or `Removed`. The first sentence is the title, what follows
the detail:

```markdown
- New Raid shoutouts. Shouts out raiders automatically in channels you moderate.
- Fixed Raid shoutouts in chats you aren't looking at. Only the chat you have open shouts out.
```

When a release is made, the files added since the last release go into its
notes under "Highlights", and the update window in the app shows them in
sections (New, Fixed, Changed, Removed). Nothing needs to be cleaned up
afterwards: a release only takes the files that are new since the last one.

Without any notes, the update window lists the release's `feat` and `fix`
commits instead.
