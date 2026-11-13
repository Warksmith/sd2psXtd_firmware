# Game2Folder wildcards and default card

Trailing `*` wildcards match any suffix (including an empty suffix). For example,
`SLUS-*` matches every ID starting with `SLUS-`, and `SLUS-123**` is equivalent to
`SLUS-123*`. Only trailing stars are supported; `?` and patterns such as `SLUS-*45`
are not supported. Matching is case-sensitive and uses the database's parent Game ID,
as existing exact mappings do.

Exact IDs take priority over wildcards, regardless of line order. Otherwise the
longest matching prefix wins. For equally specific matches, the last entry wins.
An empty mapping keeps the usual Game ID folder and suppresses the default fallback.

To use a shared card when no mapping matches, set both `UseDefaultCard=ON` and
`DefaultCard` in the relevant console section. This fallback is **off by default**;
`DefaultCard` alone does not enable it. `OFF` or an unrecognized value disables it.

```ini
[PS2]
UseDefaultCard=ON
DefaultCard=Shared
SLUS-*=USGames
SLUS-123**=Series123
SLUS-12345=Dedicated
```

Here `SLUS-12345` uses `Dedicated`, `SLUS-12399` uses `Series123`, other `SLUS-`
IDs use `USGames`, and unmatched IDs use `Shared`. These are card folder names,
not image filenames; for example, channel 1 of an image card uses
`MemoryCards/PS2/Shared/Shared-1.mcd`. Existing per-card settings and storage backends
still apply. Use short folder names (at most 31 characters).

The settings are independent in `[PS1]`, `[PS2]`, `[PROT]`, `[COH]`, and `[CONQUEST]`.
They apply when Game ID card selection runs; they do not enable Game ID switching
or replace the boot card. Without a match or an enabled, nonempty default, the usual
Game ID folder is used. A bare `*` explicitly matches all IDs, independently of the
default option.

Wildcard matching uses a single prefix comparison per entry, with no heap allocation,
regex engine, or backtracking. It uses the existing single INI scan during card selection
and adds no work to memory-card reads or writes.
