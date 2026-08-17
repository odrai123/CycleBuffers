
## `CHANGELOG.md`

```markdown
# Changelog

All notable changes to Cycle Buffers will be documented in this file.

## [1.0.0] - 2026-08-17

### Added

- Production-machine input buffer limits based on recipe cycles
- Production-machine output buffer limits based on recipe cycles
- Support for solid recipe inputs
- Support for fluid recipe inputs
- Per-save input buffer cycle setting
- Per-save output buffer cycle setting
- Generator fuel buffer limiting
- Support for solid generator fuels
- Support for fluid generator fuels
- Supplemental generator-water buffer limiting
- Per-save generator buffer cycle setting
- Independent generator buffer enable/disable option
- Live reapplication when settings are changed
- Recipe-aware limits that respect session/world recipe-cost modifiers
- Effective item stack-size clamping for production outputs
- Native supplemental-water capacity clamping for generators
- Existing-save support without deleting excess buffered items
- Compatibility with increased stack-size mods

### Notes

- Existing buffers above the configured limit are preserved and allowed to drain naturally.
- Nuclear waste inventories are not modified.
- Nuclear generators are supported by the implementation, but nuclear-specific behaviour has not yet been tested in-game.
