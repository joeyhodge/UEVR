# Townfall UE5.6 console-manager discovery

The 2026-10-08 Townfall update changes the ordering of the `r.DumpingMovie`
references. The first reference belongs to `FSlateSdfGeneratorImpl`'s constructor.
The old most-referenced-global heuristic selects its vtable instead of
`IConsoleManager::Singleton`. The reported console count is 4,294,963,768 and
the element pointer is executable code interpreted as table metadata. Iterating
that table can prevent Present from returning.

Credit to letmein for the report, submitted patch and multi-reference discovery
approach. This port adds stronger bounded memory and stock-map validation.

## Scope and safeguards

- Exact executable basename `Townfall-Win64-Shipping.exe`, case insensitive,
  and fixed-file engine major/minor 5.6. No update-specific addresses or hashes.
- At most 32 references per existing discovery string; other titles retain their
  first-reference discovery and existing lookup behavior.
- Accept only writable singleton storage in the Core image, pointing outside
  loaded images to a stock console manager. Validate executable vtable methods
  and three distinct named CVars without calling any candidate virtual function.
- Use the already tested stock-map validator for publication. Reject invalid
  table counts, capacities and arithmetic before allocating or iterating.
- Townfall lookups snapshot the header and verify the entire table range,
  including interior pages. FString comparisons are bounded and fault-guarded.
  Matching results are discarded if the header changes during lookup.
- Fuzzy lookup sorts copied names, not borrowed pointers into mutable FString
  storage. Warnings are logged once, not per frame or per bad entry.
- No rendering changes, GPU waits, UObject hooks, CVar priority changes or saved
  profile changes.

## Verification

The supplied updated EXE is UE5.6.1.0, has ordinary readable sections and matches
the complete developer PDB: GUID `BC5A8D6E-4366-F8A1-50AC-828584FBF1C8`, age 1.
It does not need a synthetic executable or a BinFold replacement PDB.

The PDB confirms the reported false reference at RVA `0x18B3BAD` belongs to
`FSlateSdfGeneratorImpl::FSlateSdfGeneratorImpl`, and the real singleton is at
RVA `0x9CF3B10`. The console map remains at manager offset `0x8`; the compared
console-manager layout and core view-init/family layouts are unchanged.

Offline tests cover the reported garbage table, wrong and unreadable singleton
slots, exact game/version gating, bounded and unterminated names, invalid capacity,
pointer overflow, and a table whose first/last pages are readable but middle page
is inaccessible. Updated-game injection, CVars, Lua/UObject access, Native Fix,
Ghost Fix, and save/level transitions still require local runtime validation.

External game-specific plugins that use fixed Townfall addresses can require
their own update; correcting UESDK discovery does not update those plugins.
