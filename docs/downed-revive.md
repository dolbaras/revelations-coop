# Revelations 1 co-op: downed / revive ("agony"), local split

Status: **implemented behind `REV1_DOWNED=1` (default off, no lobby row yet); local split checked live on the main PC on 2026-11-07 (section 6).** Pure logic and tests: `native/downed_logic.h` (the Rev2 state machine, unchanged), `native/downed1_pair.h` (two machines, one per human, local delivery), `native/downed1_pose.h` (the engine pose: plans and byte checks), `tests/test_downed.c` (the Rev2 file), `tests/test_downed_pair.c`, `tests/test_downed_pose.c` (fake engine + the facts in the executable image). Engine glue: `native/downed1.c`, `native/downed1.h`, two lines in `native/probe.c` (init and the `downed` console command).

## 1. The rule

With the rule on, in a split session (both humans are captured by `capture.c`):

* A human whose hp would reach 0 **while the other human is up** goes **DOWN** instead: hp stays at 1, he is **knocked down and lies on the floor**, his input is banned, and he **bleeds out** on a timer (default 60 s; presets 30 s / 60 s / 120 s / never: `REV1_DOWNED_BLEED=30|60|120|0`).
* The other human **holds A** within 170 units of him for 3 s: he **gets up** (the game's own get-up animation) at 35 % hp.
* **Game over** (the stock "You died / Continue?") when a human dies (the timer ran out, a death the rule did not soften) or when nobody is up.
* A hit on a DOWN human changes nothing; enemies should not pick him as a target while he lies (the get-up state's flag, section 3; static, no enemy was awake in the live runs). If the partner is not in play, a lethal hit is an ordinary death, as before.

## 2. Transport: none (local split)

Both humans run on the same PC. Each human has his own state machine (`dl_t`, `ME` = his index). What one machine sends (DOWN, DEAD, BEGIN, KEEPALIVE, CANCEL, DONE) is delivered to the other machine at once (`d1_deliver`), so the rule code is the Rev2 network code path with zero latency. The network stage (not started) can feed the same pair from the wire (section 8).

## 3. The pose: why not Raid's agony, what is used instead

Revelations 1 has a full "dying" mechanic, but only in Raid (`cPlayerDyingSyncData`, applier `0x6B6DC0`, phases 0..6). Static reading (private notes `notes\raid-reuse.md` section 10): the agony is the state 2 of the player's `cVitalControl` (`unit+0x1364`, present for campaign humans too): enter `0x6A5210` = vital state 2 + condition `unit+0xF04 = 2` and motion mode `unit+0xF08 = 2` (`FUN_006C5880`), a 7 s self-revive timer, a score call and a "rescue" prompt; revive `0x6A5290(vital, hp)`. The campaign never enters it (only when `Net_IsOnline()`).

**Live result: the campaign has no motions for mode 2.** Driving the agony offline put Parker into the bind pose (T-pose) on the Episode 1 deck. So the mod builds the pose from campaign states and motions only:

| Step | Engine call (logic thread) | Checked by bytes |
|---|---|---|
| input ban | `FUN_006A8610(unit, 1/0)`: `unit+0xF3C` bit 20, the game's own ban (lower-state input, weapon use `0x6AF1E0`) | `0x6A8610` (20 bytes) |
| fall | lower state `0x2D` = `DamageBrowState<BrowBack>` (motions `0x305` fly, `0x307` land) through `FUN_0049CD20(mgr = unit+0xE80, 0x2D, 0, 0)`; when it lands the game itself asks for `GetUpBackward 0x3B` (or `GetUpForward 0x3C`, face down) | `0x49CD20`, `0x68D7BB`, `0x68D7CB`, table words |
| lie still | the first pass that sees `0x3B`/`0x3C` plays its get-up motion (`0x315`/`0x316`) again at **speed 0** from frame 0 through the unit's vtable slot `0x68` (`0x4BBA80`: layer 0 of slot `0x6C`); the state ends only when its motion ends, so he lies; the GetUp entry sets `unit+0xE1C` bit 8, which the enemies' target filter (`0x56A7D0`) skips | `0x4BBA80`, `0x68DF1E`, `0x68DF9E`, slot `0x68` of the unit's vtable at every call |
| keep | each pass while down: the ban again; if another state took over (cutscene, grab), back into `0x3B` and frozen (at most once a second); no landing in 3 s: straight into `0x3B` | |
| get up | the frozen motion again at **speed 1**: the real getting-up, the state then ends by itself (MoveState); hp = 35 % of max through Raid's revive function `0x6A5290` (condition 1 = the campaign's low-health limp) | `0x6A5290` |
| die | ban off; lower state `0x32` = `DeadSimpleState<Normal>` (its entry is the death `0x6AE530`; `Damage()` alone does not kill: hp 0 is regenerated) | table word |

Eight code places and four state-table words are compared before the first call (`d1p_check`); a mismatch leaves the rule running without the pose (`downed1: pose OFF: <name> differs`). `REV1_DOWNED_POSE=0` switches the pose off for A/B runs.

## 4. Where the rule hooks

| Fact | Where | Used for | Status |
|---|---|---|---|
| `Damage(float)` = thiscall(unit, float), ret 4: `hp = clamp(hp - dmg, 0, max)`; hp `unit+0xE38`, max `unit+0xE40` | `0x6CA830` (8 bytes `f3 0f 10 81 38 0e 00 00`) | the hit hook: a lethal hit is cut to leave hp 1 | live: `Damage hook installed at 006ca830` |
| Regeneration `cVitalControl` (`0x6A4D70`) raises hp after a hit | `0x6A4D70` | held at 1 by the glue each pass (the HUD shows up to +6 between passes) | live: `hp 7.0` seen between passes |
| Held logical buttons of pad slot k: `sGamePad + 0x3C + 0x2CC*k + 0x15C`; A = `0x4000` | `probe.c` | the revive hold | live: Jill's A revived Parker (section 6, check 5) |
| Captured humans: `capture_get_state()` roles | `capture.h` | which unit is human 0 / 1 | live |
| Unit position `+0x40/+0x44/+0x48` | unit | the 170-unit revive range (Raid's own rescue range is 200.0, `[0x1136AA4]`) | static |

## 5. Console (lab)

`downed` / `downed status` (both humans: unit, hp, lower state, condition, mode, ban, `E1C`, vital; then the machine), `downed down <k>` (a lethal hit as the rule sees it, without an enemy), `downed pose <k> down|keep|up|die` (the pose alone, without the rule).

## 6. Live checks (main PC, 2026-11-07; Intel iGPU, window on the 2nd monitor, one copy, split armed, saves snapshotted and restored byte for byte)

Two runs, Episode 1 deck (the stock Continue), Jill = human 0 on pad 0, Parker = human 1 on pad 1, `REV1_DOWNED=1 REV1_DOWNED_BLEED=30`. Evidence = the `downed1:` log lines (quoted with their times), `downed` status lines and screenshots (lab folder `devscreens/downed/`). Lab tool: `downed down <k>` stands in for a lethal enemy hit (it calls the same `d1_absorb` as the Damage hook).

| # | Check | Result | Evidence |
|---|---|---|---|
| 1 | Hook and byte checks | **OK** | `Damage hook installed at 006ca830`; `pose calls checked (8 code places, 4 state table words)` |
| 2 | Raid agony as the pose (build ae5c339) | **unusable**: T-pose | `human 1 is DOWN ... cond(F04) 2 mode(F08) 2 ban 1 ... vital ... state 2`; screen `s1-raid-mode2-tpose.png` |
| 3 | Knock-down and lying (build 86457d5) | **OK**: fall 0x2D, lands after ~1.2 s, lies in the frozen get-up | `03:24:46.152 human 1 is DOWN ... lower 2d ... ban 1`, `03:24:47.325 human 1 pose: landed, lying (get-up frozen): ... lower 3b ... E1C 4000018a`; every status line during the 6 downs of run 2 shows `lower 3b`, `ban 1`; lying figure: `s1-lab-frozen-getup-lying-zoom.png` (the same calls by hand), his camera on the floor: `s2-jill-down-floor-camera.png` |
| 4 | Input lock | **OK** for the stick | Jill down, pad 0 stick 2.5 s: `(-220.7,-200.0,18094.7) -> (-220.7,-200.0,18094.7)`; the same stick moved her about 260 units while up (the control poll at boot: `-300.0 -> -562.6`). Pad 1 synthetic input did not drive Parker even when he was up (control failed), so Parker's lock is not evidence |
| 5 | Revive (A held by the partner) | **OK** | `03:25:08.115 human 1 is being revived`, `03:25:11.122 human 1 is up again (35% hp) ... hp 350.0/1000 ... cond(F04) 1 ... ban 0`; status during the hold `[0 me up holding 1 2765ms] [1 DOWN left 8s REVIVED]` (the bleed timer stops while held); then `lower 05`, `E1C 40000088`; repeated at 03:40:10 / 03:40:13; screens `s2-down-and-revive-jillview.png`, `s2-final-down-revive.png`, `s2-revived-standing.png` |
| 6 | Out of range | **OK** (no revive) | Jill 179 units away (`-382.0,18086.3` / `-222.5,18168.2`) held A 4.5 s: no `being revived`, he bled out at 03:36:33 |
| 7 | Bleed-out timer | **OK** | status `left 23s` ... `left 17s` ... `left 5s`; `03:26:32.573 human 1 ends (bleed-out or game over), hp was 1.0, death state 0x32 ... dead`; 30 s after DOWN in all four runs |
| 8 | Bleed-out = game over | **OK** | the pair ends both humans (`human 0 ends ... death state 0x32` in the same millisecond), death animation `s2-bleedout-death-anim.png`, stock "You died / Continue?" `s1-bleedout-gameover.png`, `s2-bleedout-gameover.png` |
| 9 | Both down = game over | **OK** | Parker DOWN; `downed down 0` -> `the rule did not absorb it (nobody up to raise him, or dead)`; Jill's death (`downed pose 0 die`) -> `03:33:06.881 human 1 ends ... death state 0x32` -> game over `s2-both-down-gameover.png` |
| 10 | Continue after a game over | **OK** | Continue -> Episode 1 start, `human 0 is unit ...`, `human 1 is unit ...`, `state reset`, both up (4 times) |
| 11 | Second DOWN after a revive | **OK** | `03:26:02.601 human 1 is DOWN` after the revive at 03:25:11 |
| 12 | Saves | **OK** | after each run: `DATA0 198f3fa8877f config 1b20c82e6f8 rev1_coop.ini absent` (the snapshot values) |

Seen on the way: while a human is down the other half shows a white placeholder box (probably the stock partner warning `uGUI_PartnerWarning`, shown for partner hp <= 25 %) ("XXXX", top right of the half); the low-hp blood / grey screen filters of the downed human cover the whole screen, not only his half (`s2-jill-down-floor-camera.png`). Both are split HUD items (B13), not the rule.

## 7. Open items (split)

* **Input lock of the second human** (pad 1): not proven (synthetic pad 1 did not drive Parker in this bench, see check 4). Next run: `REV1_PADS` or a real second pad.
* **Weapons while down**: the ban covers the lower-state input and `0x6AF1E0` (weapon use of a local human); aim/fire on the downed human was not observed separately (the run bled out during that step).
* **Cutscene / event while down**: the put-back into `0x3B` (once a second) would fight an event state (`EventWaitState`); KEEP must pause during cutscenes (the barrier's cutscene flag). Untested.
* **Real enemies**: hits on a downed human are absorbed by the rule; a damage reaction would replace the frozen state until the next put-back (one second); grabs. Untested (the deck has no awake enemy).
* **Terrain**: the knock-down moves the body about 20-80 units backwards (no hit vector is given); ledges, stairs, water, ladders untested. Under water (mode 4) DOWN should probably be the ordinary death.
* **Revive values**: 3 s hold, 170 units (Raid's own rescue range is 200.0, `[0x1136AA4]`), 35 % hp (Raid gives 20 %). The revive sets condition 1 (the campaign's low-hp limp) until the regeneration passes the threshold.
* **Reviver feedback**: no animation and no prompt (Raid's prompt `vital+0x94` offline eats the reviver's herb: `0x6B3BE0` -> `0x5C4050` while the network send returns at once).
* **HUD while down**: the hp shows 1-7 (the regeneration adds up to +6 between two passes; the glue writes 1 back); partner warning placeholder box (see above).
* **Statistics / results screen**: a bleed-out runs the campaign death on both humans (two deaths?); the results screen and achievements were not looked at.
* **Damage hook**: an inline patch of the entry while 8 unit threads may run it (the same risk `capture.c` accepts).

## 8. Open questions for the network version

1. **Hook ownership**: `world1_game.c` verifies the 16 entry bytes of `Damage` (`0x6CA830`) before it writes its core hooks; with `REV1_DOWNED=1` the entry already holds downed1's jump, so the world hooks would refuse ("core hooks cannot all be written"). One module must own the detour (chain both callbacks) or world1 must accept downed1's jump.
2. **Authority**: each PC decides DOWN / bleed-out / DEAD for its own human (the `downed_logic.h` model). The host's enemy hits on the client's human arrive through `0x49AA50` -> `Damage` on the client, where the rule absorbs them: fits. The host must never kill the client's replica from its own hp.
3. **Replica pose**: the other PC shows the downed human through the same calls on the replica (control kind 3): `0x2D` + frozen `0x315/0x316` on DOWN, speed 1 on DONE, `0x32` on DEAD. The puppet code (`puppet_game.c`: position `tp` with the mode refresh `0x6BEF50`, net pad) must stand still while the replica is down, or it fights the frozen state. Whether kind-3 units keep the frozen GetUp (their update is slot 7, `0x68CF80`) is untested.
4. **Messages**: DOWN, BEGIN, KEEPALIVE, CANCEL, DONE, DEAD of `downed_logic.h` need slots in `coop_proto` (agent A owns the protocol; request in `notes\net-proto-requests.md`). Raid's `cPlayerDyingSyncData` phases are not a usable format (gated by `Net_IsOnline`, Raid components, no campaign motions).
5. **Revive range across PCs**: the reviver's hold is measured on the reviver's PC against the replica position (latency); the 3 s hold with keepalives and the watchdog already exist in the model.
6. **Game over for both PCs**: a bleed-out runs `0x32` on the owner's human; the other PC must reach the same game over and the same Continue (B9 retry flow, role barrier).
7. **Input ban on the client**: the ban is local to the owner's PC; the replica has no input. A cutscene barrier must pause KEEP on both PCs.
8. **Reviver feedback online**: Raid's prompt could be used online only with a hook on `0x6B3BE0` (no herb loss); optional.

## 9. Decisions for the owner

* The pose is "knocked down on the back, lying still, gets up": real campaign animations, but not a crawling agony (Rev1 has none in the campaign). A reviver animation does not exist offline (Raid's prompt eats the reviver's herb when the network is off).
* The revive button (A) and range (170, Raid uses 200): the owner's preference after his first session.
* The lobby row (13 languages) and the default: not started; the rule stays off until the owner has seen it.
