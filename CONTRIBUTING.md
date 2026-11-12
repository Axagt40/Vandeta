# Contributing

Vandeta is a one-board firmware: the LilyGo T-Embed CC1101 Plus, built on Bruce
1.16.1. It is maintained by its author, and patches are welcome when they fit
that direction.

## Direction

**Pull requests are accepted at the author's discretion.** This is not a
committee project and there is no voting: the repository owner decides what
lands, what is deferred and what is out of scope. Open an issue first for
anything bigger than a bug fix - a PR that changes a menu, a console verb or the
SD layout without agreement in an issue will usually be closed with thanks.

Out of scope, regardless of patch quality: a second board, a new radio driver
the hardware does not have, cloud services, accounts or telemetry, and
reformatting unrelated files.

## Before you write code

1. Read `docs/ARCHITECTURE.md` for the module map and the one invariant that
   matters on this board (the shared SPI bus).
2. Read `docs/CONSOLE.md` if the change touches a console verb - the verb
   contract is what scripts and the mirror depend on.
3. Check `ROADMAP.md`: if your idea is one of the seven workstreams, say so in
   the issue instead of starting from scratch.

## Build and flash

`BUILD.md` has the toolchain versions, the `pio run -e lilygo-t-embed-cc1101`
command, the merge step and the `0x0` offset. A change is expected to compile
clean before it is offered.

## Test on the board

There is no host-side test suite. Verification means the board:

- Build the merged image and flash it at `0x0`.
- Exercise the changed path from the keypad **and** from the console.
- Turn on Developer Mode and check `mem` before and after: a feature that
  leaves internal DRAM below the radio guards will fail the next radio bring-up.
- Paste the `info` output and what you actually ran into the PR description.

Do not claim a hardware result you did not observe. "Compiles" is a fine thing
to say; "works" needs the board.

## Style

- Match the file you are editing. The code is clang-format'ed (`.clang-format`),
  so run it rather than reflowing by hand.
- One concern per PR. A bug fix plus a rename plus a menu reshuffle is three
  PRs.
- Keep the Bruce names: `/BruceJS`, the `bruce` verbs, the BruceNet AP. Renaming
  them breaks scripts and the upstream diff.
- No new dependencies unless the feature cannot exist without them, and never
  one with a licence that is not AGPL-3.0 compatible.
- Comments explain why, not what. The existing code follows that; match it.

## Commits and PRs

- Write the commit subject as what changed and why, in the imperative.
- Say in the PR body what you ran on the board and what you could not test.
- Every file you add is AGPL-3.0 by contributing it. Do not paste code you
  cannot licence that way, and keep third-party notices in `THIRD_PARTY.md`
  accurate if you add any.

## Reporting bugs and ideas

Use the issue templates - they ask for the board, the firmware version and the
full `info` output because those three answer most reports on their own.
Security problems go through `SECURITY.md`, not the public tracker.
