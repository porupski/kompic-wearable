# LCSC → KiCad: Complete Library Setup Guide

**Goal:** Take a list of LCSC part numbers, generate KiCad symbols + footprints + 3D models, and set up a fully portable project-local library.

Tested with: easyeda2kicad v0.8.0, KiCad 9, Linux (Ubuntu/Fedora).

---

## 1. Environment Setup (one-time)

easyeda2kicad requires Python. Use a conda/mamba environment to keep it isolated.

```bash
# Create environment
mamba create -n pcb_dev python=3.11 -y
mamba activate pcb_dev

# Install easyeda2kicad
pip install easyeda2kicad
```

Verify:

```bash
easyeda2kicad --help
```

---

## 2. Project Structure

Inside your KiCad project folder, create a library directory:

```
MyProject/
├── MyProject.kicad_pro
├── MyProject.kicad_sch
├── MyProject.kicad_pcb
├── sym-lib-table
├── fp-lib-table
├── parts.txt                    ← your parts list
└── MyProject_library/           ← created by easyeda2kicad
    ├── MyProject.kicad_sym      ← all symbols
    ├── MyProject.pretty/        ← all footprints
    └── MyProject.3dshapes/      ← all 3D models (.wrl + .step)
```

Create the empty library folder:

```bash
mkdir MyProject_library
```

---

## 3. Parts List Format

Create `parts.txt` in the project root. One LCSC ID per line, comments with `#`:

```
C2859066   # MAX30101EFD+T              OLGA-14(3.3x5.6)
C527464    # DRV2605LDGSR               VSSOP-10
# C784388  # BWGNSCNX8-8W2 (GPS ant)   FAILS — manual footprint needed
C404360    # PCF85063ATL/1,118 (RTC)    DFN-10(2.6x2.6)
```

- Commented-out lines (starting with `#`) are skipped by the import script.
- Only the first field (LCSC ID) is used — the rest is for your reference.

---

## 4. Run the Import

Activate the environment, `cd` to the project folder, and run:

```bash
mamba activate pcb_dev
cd /path/to/MyProject

grep -v '^\s*#' parts.txt | awk '{print $1}' | while read id; do
  echo "=== Fetching $id ==="
  easyeda2kicad --full --lcsc_id "$id" --output ./MyProject_library/MyProject
done
```

**Replace `MyProject_library/MyProject` with your actual folder/prefix.**

### What to expect

- First part creates the `.kicad_sym`, `.pretty/`, and `.3dshapes/` inside the library folder.
- Subsequent parts append to the same symbol lib and add footprints/models.
- Parts sharing the same package footprint (e.g., two SOT-23-3 LDOs) will warn `footprint already in .pretty` — this is fine, the symbol still gets created.
- Some parts (unusual packages, antennas) may fail with `[ERROR] Failed to fetch data from EasyEDA API` — these need manual footprints.

### If you need to re-run (overwrite existing):

```bash
grep -v '^\s*#' parts.txt | awk '{print $1}' | while read id; do
  echo "=== Fetching $id ==="
  easyeda2kicad --full --overwrite --lcsc_id "$id" --output ./MyProject_library/MyProject
done
```

---

## 5. Fix 3D Model Paths

easyeda2kicad writes relative paths like `/MyProject_library/...` in footprint files. KiCad can't resolve these. We fix them to use a KiCad environment variable.

```bash
sed -i 's|"/MyProject_library/|"${MY_3D_LIB}/|g' ./MyProject_library/MyProject.pretty/*.kicad_mod
```

**Replace:**
- `MyProject_library` with your actual folder name
- `MY_3D_LIB` with your chosen KiCad environment variable name

Verify:

```bash
grep "model" ./MyProject_library/MyProject.pretty/*.kicad_mod | head -3
```

Should show paths like `${MY_3D_LIB}/MyProject.3dshapes/SOME-FOOTPRINT.wrl`.

---

## 6. Configure KiCad

### 6a. Add the environment variable for 3D models

In KiCad: **Preferences → Configure Paths → Add**

| Name | Path |
|------|------|
| `MY_3D_LIB` | `/full/path/to/MyProject/MyProject_library` |

This is what makes the `${MY_3D_LIB}` references in the footprints resolve to actual files.

### 6b. Add library tables

Edit `sym-lib-table` in the project root:

```
(sym_lib_table
  (lib (name "MyProject")(type "KiCad")(uri "${KIPRJDIR}/MyProject_library/MyProject.kicad_sym")(options "")(descr ""))
)
```

Edit `fp-lib-table` in the project root:

```
(fp_lib_table
  (lib (name "MyProject")(type "KiCad")(uri "${KIPRJDIR}/MyProject_library/MyProject.pretty")(options "")(descr ""))
)
```

**`${KIPRJDIR}` works fine for symbol and footprint paths** — it's only the 3D model paths inside `.kicad_mod` files that need the custom environment variable.

### 6c. Verify

1. Open KiCad → Preferences → Manage Symbol Libraries → Project tab → confirm `MyProject` entry exists
2. Preferences → Manage Footprint Libraries → Project tab → confirm `MyProject` entry exists
3. Place any symbol from the library in the schematic
4. Open Footprint Properties → 3D Models tab → confirm the model preview loads

---

## 7. Portability

To share or move the project:

1. Zip the entire project folder (library is inside it — symbols, footprints, 3D models all travel together)
2. On the new machine, the recipient must add the **same environment variable** in KiCad's Configure Paths, pointing to wherever they unzipped the library folder
3. `sym-lib-table` and `fp-lib-table` use `${KIPRJDIR}` so they work automatically

---

## Quick Reference — Kompic Mk1 Actual Commands

```bash
# Activate environment
mamba activate pcb_dev
cd ~/path/to/kompic-wearable/hardware/Kompic_Mk1

# Import all parts
grep -v '^\s*#' parts.txt | awk '{print $1}' | while read id; do
  echo "=== Fetching $id ==="
  easyeda2kicad --full --lcsc_id "$id" --output ./Kompic_Mk1_library/Kompic_Mk1
done

# Fix 3D model paths
sed -i 's|"/Kompic_Mk1_library/|"${KOMPIC_MK1_LIB}/|g' ./Kompic_Mk1_library/Kompic_Mk1.pretty/*.kicad_mod

# KiCad Configure Paths:
#   KOMPIC_MK1_LIB → ~/path/to/kompic-wearable/hardware/Kompic_Mk1/Kompic_Mk1_library
```

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| 3D models don't load | Check Configure Paths env variable points to correct folder |
| `[ERROR] Failed to fetch` for a part | EasyEDA doesn't have this part — make footprint manually |
| `[WARNING] footprint already in .pretty` | Normal — two parts share same package. Symbol still created. |
| Models show in footprint editor but not 3D viewer | Close and reopen KiCad to clear cache |
| Want to update a single part | `easyeda2kicad --full --overwrite --lcsc_id CXXXXXX --output ./...` |
