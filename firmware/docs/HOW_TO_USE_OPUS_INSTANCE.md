# HOW TO USE YOUR OPUS INSTANCE FOR KOMPIC̄ FIRMWARE PORTING

## Setup (one-time)

1. Open Claude in VS Code (your Opus instance).
2. Start a new conversation.
3. **Paste the master prompt file in its entirety:**
   - File: `KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md`
   - This becomes the standing order for everything you assign to it.
4. Opus will read it and acknowledge. You're set.

---

## Daily workflow

**Each morning, give Opus one assignment file:**
- File: `KICKOFF_[MODULE]_[DESCRIPTION].md`
- Opus reads it, asks clarifying questions if needed, then executes.
- Output: one dated .md in `/docs/porting/`, driver skeleton in `/components/`, test harness in `/test/`, one commit.

**Example sequence:**

- **Day 1:** `KICKOFF_CO5300_AMOLED_DISPLAY.md` → CO5300 driver + test
- **Day 2:** `KICKOFF_CST9217_TOUCH.md` → touch driver + test
- **Day 3:** `KICKOFF_PCF85063_RTC.md` → RTC driver + test
- **Day 4:** `KICKOFF_BQ25619_CHARGER.md` → charger driver + test
- ...and so on

---

## What Opus outputs

At EOD, Opus will:
1. Create a dated .md file: `/docs/porting/[MODULE]_YYYY-MM-DD_iv7.1.f0.0.md`
2. Create driver skeleton files in `/components/[module]/`
3. Create test harness in `/test/test_[module].c`
4. Commit with a summary message
5. Post the output and ask if there are any open questions

---

## What you do with the output

**Option A: Merge into your repo**
```bash
cp -r components/* /path/to/repo/components/
cp test/test_*.c /path/to/repo/test/
cp docs/porting/*.md /path/to/repo/docs/porting/
git add -A
git commit -m "Merge Opus porting work: [MODULE], iv7.1.f0.0"
```

**Option B: Review first, then merge**
- Read the .md and test harness
- Check for hardware mismatches vs v7.2
- If OK, merge. If issues, post them to Ivan or rework with Opus.

---

## Pro tips

- **One module per conversation session.** Don't accumulate modules in one chat; one assignment per chat prevents context creep.
- **Save the .md files.** They're your documentation + proof of work. Keep them in git.
- **Profiling matters.** Opus will populate the profiling section with TBD; you fill in actual numbers when hardware arrives.
- **Test harnesses are reproducible.** Anyone can flash `test_co5300.c` to an ESP32 and verify the module works.
- **If Opus asks for clarification,** answer it. It's usually a hardware ambiguity that needs an Ivan decision.

---

## The master prompt covers

- How to structure each .md
- What sections must be present
- How to log defects
- How to commit
- What the profiling template looks like
- When to ask Ivan

Opus has everything it needs. Just hand it daily assignments and it'll deliver dated, profiled, testable code.

---

## Next steps

1. **Save these files to your project repo:**
   - `KOMPIC_MK1_FIRMWARE_PORTING_MASTER_PROMPT.md` (in root or `/docs/`)
   - `KICKOFF_CO5300_AMOLED_DISPLAY.md` (in `/docs/assignments/` or similar)

2. **Open your Opus instance in VS Code.**

3. **Paste the master prompt into a fresh chat.**
   - Opus acknowledges. You're live.

4. **Paste or reference the CO5300 kickoff.**
   - Opus starts working.
   - You check back at EOD.

5. **Tomorrow:** new kickoff for CST9217 (touch). Repeat.

---

That's it. The system is designed to be:
- **Traceable:** every module is dated, documented, committed.
- **Parallelizable:** Opus works async; you can check in whenever.
- **Testable:** each module has a standalone test harness before the full firmware exists.
- **Visible:** all work lives in .md files, not hidden in conversations.

Go.
