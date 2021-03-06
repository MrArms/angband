/**
 * \file mon-attack.c
 * \brief Monster attacks
 *
 * Monster ranged attacks - choosing an attack spell or shot and making it.
 * Monster melee attacks - monster critical blows, whether a monster 
 * attack hits, what happens when a monster attacks an adjacent player.
 *
 * Copyright (c) 1997 Ben Harrison, David Reeve Sward, Keldon Jones.
 *
 * This work is free software; you can redistribute it and/or modify it
 * under the terms of either:
 *
 * a) the GNU General Public License as published by the Free Software
 *    Foundation, version 2, or
 *
 * b) the "Angband licence":
 *    This software may be copied and distributed for educational, research,
 *    and not for profit purposes provided that this copyright and statement
 *    are included in all such copies.  Other copyrights may also apply.
 */

#include "angband.h"
#include "cave.h"
#include "init.h"
#include "mon-blow-methods.h"
#include "mon-blow-effects.h"
#include "mon-desc.h"
#include "mon-lore.h"
#include "mon-spell.h"
#include "mon-timed.h"
#include "mon-util.h"
#include "obj-identify.h"
#include "player-attack.h"
#include "player-timed.h"
#include "player-util.h"
#include "project.h"

/**
 * This file deals with monster attacks (including spells) as follows:
 *
 * Give monsters more intelligent attack/spell selection based on
 * observations of previous attacks on the player, and/or by allowing
 * the monster to "cheat" and know the player status.
 *
 * Maintain an idea of the player status, and use that information
 * to occasionally eliminate "ineffective" spell attacks.  We could
 * also eliminate ineffective normal attacks, but there is no reason
 * for the monster to do this, since he gains no benefit.
 * Note that MINDLESS monsters are not allowed to use this code.
 * And non-INTELLIGENT monsters only use it partially effectively.
 *
 * Actually learn what the player resists, and use that information
 * to remove attacks or spells before using them. 
 *
 * This has the added advantage that attacks and spells are related.
 * The "smart_learn" option means that the monster "learns" the flags
 * that should be set, and "smart_cheat" means that he "knows" them.
 * So "smart_cheat" means that the "smart" field is always up to date,
 * while "smart_learn" means that the "smart" field is slowly learned.
 * Both of them have the same effect on the "choose spell" routine.
 */

/**
 * Remove the "bad" spells from a spell list
 */
static void remove_bad_spells(struct monster *m_ptr, bitflag f[RSF_SIZE])
{
	bitflag f2[RSF_SIZE], ai_flags[OF_SIZE], ai_pflags[PF_SIZE];
	struct element_info el[ELEM_MAX];

	bool know_something = FALSE;

	/* Stupid monsters act randomly */
	if (rf_has(m_ptr->race->flags, RF_STUPID)) return;

	/* Take working copy of spell flags */
	rsf_copy(f2, f);

	/* Don't heal if full */
	if (m_ptr->hp >= m_ptr->maxhp) rsf_off(f2, RSF_HEAL);
	
	/* Don't haste if hasted with time remaining */
	if (m_ptr->m_timed[MON_TMD_FAST] > 10) rsf_off(f2, RSF_HASTE);

	/* Don't teleport to if the player is already next to us */
	if (m_ptr->cdis == 1) rsf_off(f2, RSF_TELE_TO);

	/* Update acquired knowledge */
	of_wipe(ai_flags);
	pf_wipe(ai_pflags);
	if (OPT(birth_ai_learn)) {
		size_t i;

		/* Occasionally forget player status */
		if (one_in_(100)) {
			of_wipe(m_ptr->known_pstate.flags);
			pf_wipe(m_ptr->known_pstate.pflags);
			for (i = 0; i < ELEM_MAX; i++)
				m_ptr->known_pstate.el_info[i].res_level = 0;
		}

		/* Use the memorized info */
		of_copy(ai_flags, m_ptr->known_pstate.flags);
		of_copy(ai_pflags, m_ptr->known_pstate.pflags);
		if (!of_is_empty(ai_flags) || !pf_is_empty(ai_pflags))
			know_something = TRUE;

		for (i = 0; i < ELEM_MAX; i++) {
			el[i].res_level = m_ptr->known_pstate.el_info[i].res_level;
			if (el[i].res_level != 0)
				know_something = TRUE;
		}
	}

	/* Cancel out certain flags based on knowledge */
	if (know_something)
		unset_spells(f2, ai_flags, ai_pflags, el, m_ptr->race);

	/* use working copy of spell flags */
	rsf_copy(f, f2);
}


/**
 * Determine if there is a space near the selected spot in which
 * a summoned creature can appear
 */
static bool summon_possible(int y1, int x1)
{
	int y, x;

	/* Start at the location, and check 2 grids in each dir */
	for (y = y1 - 2; y <= y1 + 2; y++) {
		for (x = x1 - 2; x <= x1 + 2; x++) {
			/* Ignore illegal locations */
			if (!square_in_bounds(cave, y, x)) continue;

			/* Only check a circular area */
			if (distance(y1, x1, y, x) > 2) continue;

			/* Hack: no summon on glyph of warding */
			if (square_iswarded(cave, y, x)) continue;

			/* If it's empty floor grid in line of sight, we're good */
			if (square_isempty(cave, y, x) && los(cave, y1, x1, y, x))
				return (TRUE);
		}
	}

	return FALSE;
}


/**
 * Have a monster choose a spell to cast.
 *
 * Note that the monster's spell list has already had "useless" spells
 * (bolts that won't hit the player, summons without room, etc.) removed.
 * Perhaps that should be done by this function.
 *
 * Stupid monsters will just pick a spell randomly.  Smart monsters
 * will choose more "intelligently".
 *
 * This function could be an efficiency bottleneck.
 */
static int choose_attack_spell(struct monster *m_ptr, bitflag f[RSF_SIZE])
{
	int num = 0;
	byte spells[RSF_MAX];

	int i;

	/* Extract all spells: "innate", "normal", "bizarre" */
	for (i = FLAG_START, num = 0; i < RSF_MAX; i++)
		if (rsf_has(f, i)) spells[num++] = i;

	/* Paranoia */
	if (num == 0) return 0;

	/* Pick at random */
	return (spells[randint0(num)]);
}


/**
 * Creatures can cast spells, shoot missiles, and breathe.
 *
 * Returns "TRUE" if a spell (or whatever) was (successfully) cast.
 *
 * XXX XXX XXX This function could use some work, but remember to
 * keep it as optimized as possible, while retaining generic code.
 *
 * Verify the various "blind-ness" checks in the code.
 *
 * XXX XXX XXX Note that several effects should really not be "seen"
 * if the player is blind.
 *
 * Perhaps monsters should breathe at locations *near* the player,
 * since this would allow them to inflict "partial" damage.
 *
 * Perhaps smart monsters should decline to use "bolt" spells if
 * there is a monster in the way, unless they wish to kill it.
 *
 * It will not be possible to "correctly" handle the case in which a
 * monster attempts to attack a location which is thought to contain
 * the player, but which in fact is nowhere near the player, since this
 * might induce all sorts of messages about the attack itself, and about
 * the effects of the attack, which the player might or might not be in
 * a position to observe.  Thus, for simplicity, it is probably best to
 * only allow "faulty" attacks by a monster if one of the important grids
 * (probably the initial or final grid) is in fact in view of the player.
 * It may be necessary to actually prevent spell attacks except when the
 * monster actually has line of sight to the player.  Note that a monster
 * could be left in a bizarre situation after the player ducked behind a
 * pillar and then teleported away, for example.
 *
 * Note that this function attempts to optimize the use of spells for the
 * cases in which the monster has no spells, or has spells but cannot use
 * them, or has spells but they will have no "useful" effect.  Note that
 * this function has been an efficiency bottleneck in the past.
 *
 * Note the special "MFLAG_NICE" flag, which prevents a monster from using
 * any spell attacks until the player has had a single chance to move.
 */
bool make_attack_spell(struct monster *m_ptr)
{
	int chance, thrown_spell, rlev, failrate;

	bitflag f[RSF_SIZE];

	monster_lore *l_ptr = get_lore(m_ptr->race);

	char m_name[80], m_poss[80], ddesc[80];

	/* Player position */
	int px = player->px;
	int py = player->py;

	/* Extract the blind-ness */
	bool blind = (player->timed[TMD_BLIND] ? TRUE : FALSE);

	/* Extract the "see-able-ness" */
	bool seen = (!blind && mflag_has(m_ptr->mflag, MFLAG_VISIBLE));

	/* Assume "normal" target */
	bool normal = TRUE;

	/* Cannot cast spells when confused */
	if (m_ptr->m_timed[MON_TMD_CONF]) return (FALSE);

	/* Cannot cast spells when nice */
	if (mflag_has(m_ptr->mflag, MFLAG_NICE)) return FALSE;

	/* Hack -- Extract the spell probability */
	chance = (m_ptr->race->freq_innate + m_ptr->race->freq_spell) / 2;

	/* Not allowed to cast spells */
	if (!chance) return FALSE;

	/* Only do spells occasionally */
	if (randint0(100) >= chance) return FALSE;

	/* Hack -- require projectable player */
	if (normal) {
		/* Check range */
		if (m_ptr->cdis > z_info->max_range)
			return FALSE;

		/* Check path */
		if (!projectable(cave, m_ptr->fy, m_ptr->fx, py, px, PROJECT_NONE))
			return FALSE;
	}

	/* Extract the monster level */
	rlev = ((m_ptr->race->level >= 1) ? m_ptr->race->level : 1);

	/* Extract the racial spell flags */
	rsf_copy(f, m_ptr->race->spell_flags);

	/* Allow "desperate" spells */
	if (rf_has(m_ptr->race->flags, RF_SMART) &&
	    m_ptr->hp < m_ptr->maxhp / 10 &&
	    randint0(100) < 50)

		/* Require intelligent spells */
		set_spells(f, RST_HASTE | RST_ANNOY | RST_ESCAPE | RST_HEAL | RST_TACTIC | RST_SUMMON);

	/* Remove the "ineffective" spells */
	remove_bad_spells(m_ptr, f);

	/* Check whether summons and bolts are worth it. */
	if (!rf_has(m_ptr->race->flags, RF_STUPID)) {
		/* Check for a clean bolt shot */
		if (test_spells(f, RST_BOLT) &&
			!projectable(cave, m_ptr->fy, m_ptr->fx, py, px, PROJECT_STOP))

			/* Remove spells that will only hurt friends */
			set_spells(f, ~RST_BOLT);

		/* Check for a possible summon */
		if (!(summon_possible(m_ptr->fy, m_ptr->fx)))

			/* Remove summoning spells */
			set_spells(f, ~RST_SUMMON);
	}

	/* No spells left */
	if (rsf_is_empty(f)) return FALSE;

	/* Get the monster name (or "it") */
	monster_desc(m_name, sizeof(m_name), m_ptr, MDESC_STANDARD);

	/* Get the monster possessive ("his"/"her"/"its") */
	monster_desc(m_poss, sizeof(m_poss), m_ptr, MDESC_PRO_VIS | MDESC_POSS);

	/* Get the "died from" name */
	monster_desc(ddesc, sizeof(ddesc), m_ptr, MDESC_DIED_FROM);

	/* Choose a spell to cast */
	thrown_spell = choose_attack_spell(m_ptr, f);

	/* Abort if no spell was chosen */
	if (!thrown_spell) return FALSE;

	/* If we see an unaware monster try to cast a spell, become aware of it */
	if (mflag_has(m_ptr->mflag, MFLAG_UNAWARE))
		become_aware(m_ptr);

	/* Calculate spell failure rate */
	failrate = 25 - (rlev + 3) / 4;
	if (m_ptr->m_timed[MON_TMD_FEAR])
		failrate += 20;

	/* Stupid monsters will never fail (for jellies and such) */
	if (rf_has(m_ptr->race->flags, RF_STUPID))
		failrate = 0;

	/* Check for spell failure (innate attacks never fail) */
	if ((thrown_spell >= MIN_NONINNATE_SPELL) && (randint0(100) < failrate)) {
		/* Message */
		msg("%s tries to cast a spell, but fails.", m_name);

		return TRUE;
	}

	/* Cast the spell. */
	disturb(player, 1);

	/* Special case RSF_HASTE until TMD_* and MON_TMD_* are rationalised */
	if (thrown_spell == RSF_HASTE) {
		if (blind)
			msg("%s mumbles.", m_name);
		else
			msg("%s concentrates on %s body.", m_name, m_poss);

		(void)mon_inc_timed(m_ptr, MON_TMD_FAST, 50, 0, FALSE);
	} else {
		do_mon_spell(thrown_spell, m_ptr, seen);
	}

	/* Remember what the monster did to us */
	if (seen) {
		rsf_on(l_ptr->spell_flags, thrown_spell);

		/* Innate spell */
		if (thrown_spell < MIN_NONINNATE_SPELL) {
			if (l_ptr->cast_innate < MAX_UCHAR)
				l_ptr->cast_innate++;
		} else {
		/* Bolt or Ball, or Special spell */
			if (l_ptr->cast_spell < MAX_UCHAR)
				l_ptr->cast_spell++;
		}
	}

	/* Always take note of monsters that kill you */
	if (player->is_dead && (l_ptr->deaths < MAX_SHORT)) {
		l_ptr->deaths++;
	}

	/* Record any new info */
	lore_update(m_ptr->race, l_ptr);

	/* A spell was cast */
	return TRUE;
}



/**
 * Critical blow.  All hits that do 95% of total possible damage,
 * and which also do at least 20 damage, or, sometimes, N damage.
 * This is used only to determine "cuts" and "stuns".
 */
static int monster_critical(random_value dice, int rlev, int dam)
{
	int max = 0;
	int total = randcalc(dice, rlev, MAXIMISE);

	/* Must do at least 95% of perfect */
	if (dam < total * 19 / 20) return (0);

	/* Weak blows rarely work */
	if ((dam < 20) && (randint0(100) >= dam)) return (0);

	/* Perfect damage */
	if (dam == total) max++;

	/* Super-charge */
	if (dam >= 20)
		while (randint0(100) < 2) max++;

	/* Critical damage */
	if (dam > 45) return (6 + max);
	if (dam > 33) return (5 + max);
	if (dam > 25) return (4 + max);
	if (dam > 18) return (3 + max);
	if (dam > 11) return (2 + max);
	return (1 + max);
}

/**
 * Determine if a monster attack against the player succeeds.
 */
bool check_hit(struct player *p, int power, int level)
{
	int chance, ac;

	/* Calculate the "attack quality" */
	chance = (power + (level * 3));

	/* Total armor */
	ac = p->state.ac + p->state.to_a;

	/* If the monster checks vs ac, the player learns ac bonuses */
	equip_notice_on_defend(p);

	/* Check if the player was hit */
	return test_hit(chance, ac, TRUE);
}

/**
 * Calculate how much damage remains after armor is taken into account
 * (does for a physical attack what adjust_dam does for an elemental attack).
 */
int adjust_dam_armor(int damage, int ac)
{
	return damage - (damage * ((ac < 240) ? ac : 240) / 400);
}

/**
 * Attack the player via physical attacks.
 */
bool make_attack_normal(struct monster *m_ptr, struct player *p)
{
	monster_lore *l_ptr = get_lore(m_ptr->race);
	int ap_cnt;
	int k, tmp, ac, rlev;
	char m_name[80];
	char ddesc[80];
	bool blinked;

	/* Not allowed to attack */
	if (rf_has(m_ptr->race->flags, RF_NEVER_BLOW)) return (FALSE);

	/* Total armor */
	ac = p->state.ac + p->state.to_a;

	/* Extract the effective monster level */
	rlev = ((m_ptr->race->level >= 1) ? m_ptr->race->level : 1);


	/* Get the monster name (or "it") */
	monster_desc(m_name, sizeof(m_name), m_ptr, MDESC_STANDARD);

	/* Get the "died from" information (i.e. "a kobold") */
	monster_desc(ddesc, sizeof(ddesc), m_ptr, MDESC_SHOW | MDESC_IND_VIS);

	/* Assume no blink */
	blinked = FALSE;

	/* Scan through all blows */
	for (ap_cnt = 0; ap_cnt < z_info->mon_blows_max; ap_cnt++) {
		bool visible = FALSE;
		bool obvious = FALSE;
		bool do_break = FALSE;

		int power = 0;
		int damage = 0;
		int do_cut = 0;
		int do_stun = 0;
		int sound_msg = MSG_GENERIC;

		const char *act = NULL;

		/* Extract the attack infomation */
		int effect = m_ptr->race->blow[ap_cnt].effect;
		int method = m_ptr->race->blow[ap_cnt].method;
		random_value dice = m_ptr->race->blow[ap_cnt].dice;

		/* Hack -- no more attacks */
		if (!method) break;

		/* Handle "leaving" */
		if (p->is_dead || p->upkeep->generate_level) break;

		/* Extract visibility (before blink) */
		if (mflag_has(m_ptr->mflag, MFLAG_VISIBLE)) visible = TRUE;

		/* Extract visibility from carrying light */
		if (rf_has(m_ptr->race->flags, RF_HAS_LIGHT)) visible = TRUE;

		/* Extract the attack "power" */
		power = monster_blow_effect_power(effect);

		/* Monster hits player */
		if (!effect || check_hit(p, power, rlev)) {
			melee_effect_handler_f effect_handler;

			/* Always disturbing */
			disturb(p, 1);

			/* Hack -- Apply "protection from evil" */
			if (p->timed[TMD_PROTEVIL] > 0) {
				/* Learn about the evil flag */
				if (mflag_has(m_ptr->mflag, MFLAG_VISIBLE))
					rf_on(l_ptr->flags, RF_EVIL);

				if (rf_has(m_ptr->race->flags, RF_EVIL) && p->lev >= rlev &&
				    randint0(100) + p->lev > 50) {
					/* Message */
					msg("%s is repelled.", m_name);

					/* Hack -- Next attack */
					continue;
				}
			}

			/* Describe the attack method */
			act = monster_blow_method_action(method);
			do_cut = monster_blow_method_cut(method);
			do_stun = monster_blow_method_stun(method);
			sound_msg = monster_blow_method_message(method);

			/* Message */
			if (act)
				msgt(sound_msg, "%s %s", m_name, act);

			/* Hack -- assume all attacks are obvious */
			obvious = TRUE;

			/* Roll dice */
			damage = randcalc(dice, rlev, RANDOMISE);

			/* Perform the actual effect. */
			effect_handler = melee_handler_for_blow_effect(effect);
			if (effect_handler != NULL) {
				melee_effect_handler_context_t context = {
					p,
					m_ptr,
					rlev,
					method,
					ac,
					ddesc,
					obvious,
					blinked,
					do_break,
					damage,
				};

				effect_handler(&context);

				/* Save any changes made in the handler for later use. */
				obvious = context.obvious;
				blinked = context.blinked;
				damage = context.damage;
			} else {
				msg("ERROR: Effect handler not found for %d.", effect);
			}


			/* Hack -- only one of cut or stun */
			if (do_cut && do_stun) {
				/* Cancel cut */
				if (randint0(100) < 50)
					do_cut = 0;

				/* Cancel stun */
				else
					do_stun = 0;
			}

			/* Handle cut */
			if (do_cut) {
				/* Critical hit (zero if non-critical) */
				tmp = monster_critical(dice, rlev, damage);

				/* Roll for damage */
				switch (tmp) {
					case 0: k = 0; break;
					case 1: k = randint1(5); break;
					case 2: k = randint1(5) + 5; break;
					case 3: k = randint1(20) + 20; break;
					case 4: k = randint1(50) + 50; break;
					case 5: k = randint1(100) + 100; break;
					case 6: k = 300; break;
					default: k = 500; break;
				}

				/* Apply the cut */
				if (k) (void)player_inc_timed(p, TMD_CUT, k, TRUE, TRUE);
			}

			/* Handle stun */
			if (do_stun) {
				/* Critical hit (zero if non-critical) */
				tmp = monster_critical(dice, rlev, damage);

				/* Roll for damage */
				switch (tmp) {
					case 0: k = 0; break;
					case 1: k = randint1(5); break;
					case 2: k = randint1(10) + 10; break;
					case 3: k = randint1(20) + 20; break;
					case 4: k = randint1(30) + 30; break;
					case 5: k = randint1(40) + 40; break;
					case 6: k = 100; break;
					default: k = 200; break;
				}

				/* Apply the stun */
				if (k)
					(void)player_inc_timed(p, TMD_STUN, k, TRUE, TRUE);
			}
		} else {
			/* Visible monster missed player, so notify if appropriate. */
			if (mflag_has(m_ptr->mflag, MFLAG_VISIBLE) &&
				monster_blow_method_miss(method)) {
				/* Disturbing */
				disturb(p, 1);
				msg("%s misses you.", m_name);
			}
		}

		/* Analyze "visible" monsters only */
		if (visible) {
			/* Count "obvious" attacks (and ones that cause damage) */
			if (obvious || damage || (l_ptr->blows[ap_cnt].times_seen > 10)) {
				/* Count attacks of this type */
				if (l_ptr->blows[ap_cnt].times_seen < MAX_UCHAR)
					l_ptr->blows[ap_cnt].times_seen++;
			}
		}

		/* Skip the other blows if necessary */
		if (do_break) break;
	}

	/* Blink away */
	if (blinked) {
		char dice[5];
		msg("There is a puff of smoke!");
		strnfmt(dice, sizeof(dice), "%d", z_info->max_sight * 2 + 5);
		effect_simple(EF_TELEPORT, dice, 0, 0, 0, NULL);
	}

	/* Always notice cause of death */
	if (p->is_dead && (l_ptr->deaths < MAX_SHORT))
		l_ptr->deaths++;

	/* Learn lore */
	lore_update(m_ptr->race, l_ptr);

	/* Assume we attacked */
	return (TRUE);
}



/* Test functions */
bool (*testfn_make_attack_normal)(struct monster *m, struct player *p) = make_attack_normal;
