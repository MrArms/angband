/**
 * \file project-obj.c
 * \brief projection effects on objects
 *
 * Copyright (c) 1997 Ben Harrison, James E. Wilson, Robert A. Koeneke
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
#include "mon-util.h"
#include "obj-chest.h"
#include "obj-desc.h"
#include "obj-gear.h"
#include "obj-identify.h"
#include "obj-ignore.h"
#include "obj-pile.h"
#include "obj-tval.h"
#include "obj-util.h"

/**
 * Destroys a type of item on a given percent chance.
 * The chance 'cperc' is in hundredths of a percent (1-in-10000)
 * Note that missiles are no longer necessarily all destroyed
 *
 * Returns number of items destroyed.
 */
int inven_damage(struct player *p, int type, int cperc)
{
	int j, k, amt;
	struct object *obj;
	char o_name[80];
	bool damage;

	/* No chance means no damage */
	if (cperc <= 0)
		return 0;

	/* Count the casualties */
	k = 0;

	/* Scan through the gear */
	for (obj = p->gear; obj; obj = obj->next) {
		if (object_is_equipped(p->body, obj))
			continue;

		/* Hack -- for now, skip artifacts */
		if (obj->artifact) continue;

		/* Give this item slot a shot at death if it is vulnerable */
		if ((obj->el_info[type].flags & EL_INFO_HATES) &&
			!(obj->el_info[type].flags & EL_INFO_IGNORE)) {
			/* Chance to destroy this item */
			int chance = cperc;

			/* Track if it is damaged instead of destroyed */
			damage = FALSE;

			/* Analyze the type to see if we just damage it
			 * - we also check for rods to reduce chance */
			if (tval_is_weapon(obj) && !tval_is_ammo(obj)) {
				/* Chance to damage it */
				if (randint0(10000) < cperc) {
					/* Damage the item */
					obj->to_h--;
					obj->to_d--;

					/* Damaged! */
					damage = TRUE;
				} else
					continue;
			} else if (tval_is_armor(obj)) {
				/* Chance to damage it */
				if (randint0(10000) < cperc) {
					/* Damage the item */
					obj->to_a--;

					/* Damaged! */
					damage = TRUE;
				} else
					continue;
			} else if (tval_is_rod(obj)) {
				chance = (chance / 4);
			}


			/* Damage instead of destroy */
			if (damage) {
				p->upkeep->update |= (PU_BONUS);
				p->upkeep->redraw |= (PR_EQUIP);

				/* Casualty count */
				amt = obj->number;
			} else
				/* ... or count the casualties */
				for (amt = j = 0; j < obj->number; ++j)
					if (randint0(10000) < chance) amt++;

			/* Some casualities */
			if (amt) {
				struct object *destroyed;

				/* Get a description */
				object_desc(o_name, sizeof(o_name), obj, ODESC_BASE);

				/* Message */
				msgt(MSG_DESTROY, "%sour %s (%c) %s %s!",
				           ((obj->number > 1) ?
				            ((amt == obj->number) ? "All of y" :
				             (amt > 1 ? "Some of y" : "One of y")) : "Y"),
				           o_name, gear_to_label(obj),
				           ((amt > 1) ? "were" : "was"),
					   (damage ? "damaged" : "destroyed"));

				/* Damage already done? */
				if (damage)
					continue;

				/* Reduce charges if some devices are destroyed */
				reduce_charges(obj, amt);

				/* Destroy "amt" items */
				destroyed = gear_object_for_use(obj, 1, FALSE);
				object_delete(destroyed);

				/* Count the casualties */
				k += amt;
			}
		}
	}

	/* Return the casualty count */
	return (k);
}

/**
 * ------------------------------------------------------------------------
 * Object handlers
 * ------------------------------------------------------------------------ */

typedef struct project_object_handler_context_s {
	const int who;
	const int r;
	const int y;
	const int x;
	const int dam;
	const int type;
	const struct object *obj;
	bool obvious;
	bool do_kill;
	bool ignore;
	const char *note_kill;
} project_object_handler_context_t;
typedef void (*project_object_handler_f)(project_object_handler_context_t *);

/**
 * Project an effect onto an object.
 *
 * \param context is the project_o context.
 * \param element is for elements that will destroy an object, or that it will
 * ignore.
 * \param singular_verb is the verb that is displayed when one object is
 * destroyed.
 * \param plural_verb is the verb that is displayed in multiple objects are
 * destroyed.
 */
static void project_object_elemental(project_object_handler_context_t *context,
									 int element, const char *singular_verb,
									 const char *plural_verb)
{
	if (context->obj->el_info[element].flags & EL_INFO_HATES) {
		context->do_kill = TRUE;
		context->note_kill = VERB_AGREEMENT(context->obj->number,
											singular_verb, plural_verb);
		context->ignore = (context->obj->el_info[element].flags &
						   EL_INFO_IGNORE) ? TRUE : FALSE;
	}
}

/* Acid -- Lots of things */
static void project_object_handler_ACID(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_ACID, "melts", "melt");
}

/* Elec -- Rings and Wands */
static void project_object_handler_ELEC(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_ELEC, "is destroyed", "are destroyed");
}

/* Fire -- Flammable objects */
static void project_object_handler_FIRE(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_FIRE, "burns up", "burn up");
}

/* Cold -- potions and flasks */
static void project_object_handler_COLD(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_COLD, "shatters", "shatter");
}

static void project_object_handler_POIS(project_object_handler_context_t *context)
{
}

static void project_object_handler_LIGHT(project_object_handler_context_t *context)
{
}

static void project_object_handler_DARK(project_object_handler_context_t *context)
{
}

/* Sound -- potions and flasks */
static void project_object_handler_SOUND(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_SOUND, "shatters", "shatter");
}

/* Shards -- potions and flasks */
static void project_object_handler_SHARD(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_SHARD, "shatters", "shatter");
}

static void project_object_handler_NEXUS(project_object_handler_context_t *context)
{
}

static void project_object_handler_NETHER(project_object_handler_context_t *context)
{
}

static void project_object_handler_CHAOS(project_object_handler_context_t *context)
{
}

static void project_object_handler_DISEN(project_object_handler_context_t *context)
{
}

static void project_object_handler_WATER(project_object_handler_context_t *context)
{
}

/* Ice -- potions and flasks */
static void project_object_handler_ICE(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_ICE, "shatters", "shatter");
}

static void project_object_handler_GRAVITY(project_object_handler_context_t *context)
{
}

static void project_object_handler_INERTIA(project_object_handler_context_t *context)
{
}

/* Force -- potions and flasks */
static void project_object_handler_FORCE(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_FORCE, "shatters", "shatter");
}

static void project_object_handler_TIME(project_object_handler_context_t *context)
{
}

/* Fire + Elec */
static void project_object_handler_PLASMA(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_FIRE, "burns up", "burn up");
	project_object_elemental(context, ELEM_ELEC, "is destroyed", "are destroyed");
}

/* Fire + Cold */
static void project_object_handler_METEOR(project_object_handler_context_t *context)
{
	project_object_elemental(context, ELEM_FIRE, "burns up", "burn up");
	project_object_elemental(context, ELEM_COLD, "shatters", "shatter");
}

static void project_object_handler_MISSILE(project_object_handler_context_t *context)
{
}

/* Mana -- destroys everything */
static void project_object_handler_MANA(project_object_handler_context_t *context)
{
	context->do_kill = TRUE;
	context->note_kill = VERB_AGREEMENT(context->obj->number, "is destroyed", "are destroyed");
}

/* Holy Orb -- destroys cursed non-artifacts */
static void project_object_handler_HOLY_ORB(project_object_handler_context_t *context)
{
	if (cursed_p(context->obj->flags)) {
		context->do_kill = TRUE;
		context->note_kill = VERB_AGREEMENT(context->obj->number, "is destroyed", "are destroyed");
	}
}

static void project_object_handler_ARROW(project_object_handler_context_t *context)
{
}

static void project_object_handler_LIGHT_WEAK(project_object_handler_context_t *context)
{
}

static void project_object_handler_DARK_WEAK(project_object_handler_context_t *context)
{
}

static void project_object_handler_KILL_WALL(project_object_handler_context_t *context)
{
}

/* Unlock chests */
static void project_object_handler_KILL_DOOR(project_object_handler_context_t *context)
{
	/* Chests are noticed only if trapped or locked */
	if (is_locked_chest(context->obj)) {
		/* Disarm or Unlock */
		unlock_chest((object_type * const)context->obj);

		/* Identify */
		object_notice_everything((object_type * const)context->obj);

		/* Notice */
		if (context->obj->marked > MARK_UNAWARE && !ignore_item_ok(context->obj)) {
			msg("Click!");
			context->obvious = TRUE;
		}
	}
}

/* Unlock chests */
static void project_object_handler_KILL_TRAP(project_object_handler_context_t *context)
{
	/* Chests are noticed only if trapped or locked */
	if (is_locked_chest(context->obj)) {
		/* Disarm or Unlock */
		unlock_chest((object_type * const)context->obj);

		/* Identify */
		object_notice_everything((object_type * const)context->obj);

		/* Notice */
		if (context->obj->marked > MARK_UNAWARE && !ignore_item_ok(context->obj)) {
			msg("Click!");
			context->obvious = TRUE;
		}
	}
}

static void project_object_handler_MAKE_DOOR(project_object_handler_context_t *context)
{
}

static void project_object_handler_MAKE_TRAP(project_object_handler_context_t *context)
{
}

static const project_object_handler_f object_handlers[] = {
	#define ELEM(a, b, c, d, e, f, g, h, i, col) project_object_handler_##a,
	#include "list-elements.h"
	#undef ELEM
	#define PROJ_ENV(a, col) project_object_handler_##a,
	#include "list-project-environs.h"
	#undef PROJ_ENV
	#define PROJ_MON(a, obv) NULL, 
	#include "list-project-monsters.h"
	#undef PROJ_MON
	NULL
};

/**
 * Called from project() to affect objects
 *
 * Called for projections with the PROJECT_ITEM flag set, which includes
 * beam, ball and breath effects.
 *
 * \param who is the monster list index of the caster
 * \param r is the distance from the centre of the effect
 * \param y
 * \param x the coordinates of the grid being handled
 * \param dam is the "damage" from the effect at distance r from the centre
 * \param typ is the projection (GF_) type
 * \return whether the effects were obvious
 *
 * Note that this function determines if the player can see anything that
 * happens by taking into account: blindness, line-of-sight, and illumination.
 *
 * Hack -- effects on objects which are memorized but not in view are also seen.
 */
bool project_o(int who, int r, int y, int x, int dam, int typ)
{
	struct object *obj = square_object(cave, y, x);
	bool obvious = FALSE;

	/* Scan all objects in the grid */
	while (obj) {
		bool ignore = FALSE;
		bool do_kill = FALSE;
		const char *note_kill = NULL;
		struct object *next = obj->next;
		project_object_handler_context_t context = {
			who,
			r,
			y,
			x,
			dam,
			typ,
			obj,
			obvious,
			do_kill,
			ignore,
			note_kill,
		};
		project_object_handler_f object_handler = object_handlers[typ];

		if (object_handler != NULL)
			object_handler(&context);

		obvious = context.obvious;
		do_kill = context.do_kill;
		ignore = context.ignore;
		note_kill = context.note_kill;

		/* Attempt to destroy the object */
		if (do_kill) {
			char o_name[80];

			/* Effect "observed" */
			if (obj->marked && !ignore_item_ok(obj)) {
				obvious = TRUE;
				object_desc(o_name, sizeof(o_name), obj, ODESC_BASE);
			}

			/* Artifacts, and other objects, get to resist */
			if (obj->artifact || ignore) {
				/* Observe the resist */
				if (obj->marked && !ignore_item_ok(obj))
					msg("The %s %s unaffected!", o_name,
						VERB_AGREEMENT(obj->number, "is", "are"));
			} else if (obj->mimicking_m_idx) {
				/* Reveal mimics */
				become_aware(cave_monster(cave, obj->mimicking_m_idx));
			} else {
				/* Describe if needed */
				if (obj->marked && note_kill && !ignore_item_ok(obj))
					msgt(MSG_DESTROY, "The %s %s!", o_name, note_kill);

				/* Delete the object */
				square_excise_object(cave, y, x, obj);
				object_delete(obj);

				/* Redraw */
				square_light_spot(cave, y, x);
			}
		}

		/* Next object */
		obj = next;
	}

	/* Return "Anything seen?" */
	return (obvious);
}
