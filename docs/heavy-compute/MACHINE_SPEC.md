# Big-machine spec (generated from inventory.yaml by scripts/heavy/render_docs.py)

Derived from the inventory's per-item ESTIMATES (largest single item: 24 GB VRAM, 64 GB RAM, 100 GB working disk; datasets 56.9 GB).
Tier verdicts use the same rule as `scripts/heavy/check_machine.py`. Items that are
NOT_IMPLEMENTED or have a TODO command are listed separately: no machine runs them yet.

| Tier | Example | VRAM | RAM | cores | disk | runnable | blocked | cost ESTIMATE for the 19-49 GPU-h of summable GPU items |
|---|---|---|---|---|---|---|---|---|
| T0 local (this laptop) | Intel Core Ultra 5 226V, no NVIDIA GPU | 0 GB | 15.2 GB | 8 | 9.7 GB | 2 | 12 | n/a |
| T1 minimum | 1x 24 GB GPU (RTX 4090 / L4 / A10G class), 64 GB RAM, 16 cores, 500 GB NVMe | 24 GB | 64 GB | 16 | 500 GB | 14 | 0 | $13-$73 (at $0.7-1.5/h, verify with provider) |
| T2 recommended | 1x 48 GB GPU (L40S / RTX 6000 Ada class), 128 GB RAM, 32 cores, 1 TB NVMe | 48 GB | 128 GB | 32 | 1000 GB | 14 | 0 | $28-$146 (at $1.5-3.0/h, verify with provider) |
| T3 headroom | 1x 80 GB GPU (A100/H100 80 GB class), 256 GB RAM, 64 cores, 2 TB NVMe | 80 GB | 256 GB | 64 | 2000 GB | 14 | 0 | $48-$244 (at $2.5-5.0/h, verify with provider) |

### T0 local (this laptop)

- Runnable (2): P1-02, P3-04
- Blocked (12): P0-01: needs >= 1 GB VRAM, found 0; needs ~16 GB RAM, found 15; needs ~40 GB disk, found 10 free; P0-02: needs >= 1 GB VRAM, found 0; P2-01: needs >= 4 GB VRAM, found 0; needs ~16 GB RAM, found 15; P2-05: needs ~16 GB RAM, found 15; needs ~25 GB disk, found 10 free; P2-06: needs ~16 GB RAM, found 15; P3-01: needs >= 2 GB VRAM, found 0; needs ~12 GB disk, found 10 free; P3-02: needs >= 6 GB VRAM, found 0; P3-05: needs >= 2 GB VRAM, found 0; needs ~16 GB RAM, found 15; needs ~15 GB disk, found 10 free; P3-08: needs >= 8 GB VRAM, found 0; needs ~32 GB RAM, found 15; P3-09: needs ~16 GB RAM, found 15; P3-11: needs ~16 GB RAM, found 15; P3-12: needs >= 4 GB VRAM, found 0

### T1 minimum

- Runnable (14): P0-01, P0-02, P1-02, P2-01, P2-05, P2-06, P3-01, P3-02, P3-04, P3-05, P3-08, P3-09, P3-11, P3-12
- Blocked (0): -

### T2 recommended

- Runnable (14): P0-01, P0-02, P1-02, P2-01, P2-05, P2-06, P3-01, P3-02, P3-04, P3-05, P3-08, P3-09, P3-11, P3-12
- Blocked (0): -

### T3 headroom

- Runnable (14): P0-01, P0-02, P1-02, P2-01, P2-05, P2-06, P3-01, P3-02, P3-04, P3-05, P3-08, P3-09, P3-11, P3-12
- Blocked (0): -

### Not runnable on any tier yet

- NOT_IMPLEMENTED (18): P1-01, P2-03, P2-04, P3-03, P3-10, P4-01, P4-02, P6-01, P7-01, P7-03, P7-04, P8-01, P8-02, P8-03, P8-04, P9-01, P9-02, P9-03
- Implemented/partial but command TODO (5): P2-02, P3-06, P3-07, P6-02, P7-02

## Recommendation

**T1 is the minimum** that runs every currently implemented GPU item (largest VRAM need among
runnable items is 8 GB; 24 GB covers the splat training estimates of 16 GB once Part 4 exists).
**T2 is recommended**: P3-10 stress (24 GB VRAM, 64 GB RAM), OpenMVS full-res densify (64 GB RAM)
and the end-to-end runs (64 GB RAM, 100 GB disk) fit with margin, and 1 TB NVMe holds all
datasets (56.9 GB) plus several full runs. All resource figures are ESTIMATES (see inventory
`basis`); re-derive after the first T1/T2 run fills RESULTS_TEMPLATE.md.
