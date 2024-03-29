// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/cbasetypes.h"
#include "../common/timer.h"
#include "../common/nullpo.h"
#include "../common/malloc.h"
#include "../common/mapindex.h"
#include "../common/showmsg.h"
#include "../common/ers.h"
#include "../common/strlib.h"
#include "../common/utils.h"
#include "../common/socket.h"

#include "map.h"
#include "duel.h"
#include "guild.h"
#include "storage.h"
#include "battle.h"
#include "npc.h"
#include "pc.h"
#include "instance.h"
#include "intif.h"
#include "channel.h"
#include "log.h"
#include "trade.h"

#include <stdlib.h>


static DBMap* guild_db; // int guild_id -> struct guild*
static DBMap* castle_db; // int castle_id -> struct guild_castle*
static DBMap* guild_expcache_db; // uint32 char_id -> struct guild_expcache*
static DBMap* guild_infoevent_db; // int guild_id -> struct eventlist*

struct eventlist {
	char name[EVENT_NAME_LENGTH];
	struct eventlist *next;
};

//Constant related to the flash of the Guild EXP cache
#define GUILD_SEND_XY_INTERVAL	5000 // Interval of sending coordinates and HP
#define GUILD_PAYEXP_INTERVAL 10000 //Interval (maximum survival time of the cache, in milliseconds)
#define GUILD_PAYEXP_LIST 8192 //The maximum number of cache

//Guild EXP cache
struct guild_expcache {
	int guild_id, account_id, char_id;
	uint64 exp;
};
static struct eri *expcache_ers; //For handling of guild exp payment.

#define MAX_GUILD_SKILL_REQUIRE 5
struct{
	int id;
	int max;
	struct{
		short id;
		short lv;
	}need[MAX_GUILD_SKILL_REQUIRE];
} guild_skill_tree[MAX_GUILDSKILL];

int guild_payexp_timer(int tid, unsigned int tick, int id, intptr_t data);
static int guild_send_xy_timer(int tid, unsigned int tick, int id, intptr_t data);

/* guild flags cache */
struct npc_data **guild_flags;
unsigned short guild_flags_count;

/**
 * Get guild skill index in guild_skill_tree
 * @param skill_id
 * @return Index in skill_tree or -1
 **/
static short guild_skill_get_index(uint16 skill_id) {
	if (!SKILL_CHK_GUILD(skill_id))
		return -1;
	skill_id -= GD_SKILLBASE;
	if (skill_id >= MAX_GUILDSKILL)
		return -1;
	return skill_id;
}

/*==========================================
 * Guild Wars
 *------------------------------------------*/
bool guild_isatwar(int guild_id)
{
	struct guild *g;

	if( guild_id == 0 ) return false;

	if( battle_config.guild_wars && (g = guild_search(guild_id)) != NULL && g->war )
		return true;

	return false;
}

bool guild_canescape(struct map_session_data *sd)
{
	if( !guild_isatwar(sd->status.guild_id) )
		return true;

	if( DIFF_TICK(gettick(), sd->canescape_tick) > 10000 )
		return true;

	return false;
}

bool guild_isenemy(int guild_id, int tguild_id)
{
	struct guild *g = guild_search(guild_id), *tg;
	int i;

	if( !(battle_config.guild_wars && g && g->war && guild_id && tguild_id) )
		return false;

	ARR_FIND(0, MAX_GUILDALLIANCE, i, g->alliance[i].guild_id == tguild_id);
	if( i >= MAX_GUILDALLIANCE ) return false;

	tg = guild_search(tguild_id);

	if( tg && g->alliance[i].war && g->alliance[i].opposition )
	{
		g->war_tick = tg->war_tick = last_tick + 600;
		return true;
	}

	return false;
}

bool guild_wardamage(struct map_session_data *sd)
{
	struct guild *g = sd->guild;

	if( !(battle_config.guild_wars && g) || sd->state.pvpmode || sd->duel_group > 0 || map_flag_noguildwar(sd->bl.m) || agit_flag || agit2_flag )
		return false;

	return g->war;
}

bool guild_can_breakwar(int guild_id, int tguild_id)
{
	struct guild *g = guild_search(guild_id);
	int i;

	if( !battle_config.guild_wars || !g ) return true;

	ARR_FIND(0, MAX_GUILDALLIANCE, i, g->alliance[i].guild_id == tguild_id);
	if( i >= MAX_GUILDALLIANCE ) return true; // ??

	if( g->alliance[i].war && g->alliance[i].opposition	&& DIFF_TICK(g->war_tick, last_tick) > 0 )
		return false;

	return true;
}

void guild_war_init(struct guild *g, struct guild *eg, bool starting)
{
	int i, len;
	struct map_session_data *pl_sd;
	char output[256];
	sprintf(output, "Your Guild is at War with %s.", eg->name);

	len = strlen(output);

	if( starting )
	{
		for( i = 0; i < g->max_member; i++ )
		{
			if( (pl_sd = g->member[i].sd ) == NULL )
				continue;

			clif_disp_onlyself(pl_sd, output, len);

			if( g->war || map_flag_noguildwar(pl_sd->bl.m) )
				continue;

			if( pl_sd->state.pvpmode )
				pc_pvpmodeoff(pl_sd, 1, 1); // Cannot be in PVPMODE at GuildWar
			else if( pl_sd->duel_group )
				duel_leave(pl_sd->duel_group, pl_sd);
			
			clif_map_property(pl_sd, MAPPROPERTY_FREEPVPZONE);
		}

		g->war_tick = last_tick + 600; // 10 Minutes Cannot remove the Opposition
	}

	g->war = true;
}

void guild_war_end(struct guild *g, struct guild *eg, bool surrender)
{
	int i, j, len;
	struct map_session_data *pl_sd;
	char output[256];

	if( !g || !eg )
		return;

	if( surrender )
		sprintf(output, "Your Guild has surrender to %s.", eg->name);
	else
		sprintf(output, "Your Guild has won the War against %s.", eg->name);

	len = strlen(output);

	for( i = j = 0; i < MAX_GUILDALLIANCE; i++ )
	{
		if( g->alliance[i].guild_id <= 0 || g->alliance[i].opposition != 1 )
			continue;

		if( g->alliance[i].guild_id == eg->guild_id )
			g->alliance[i].war = false;
		else if( g->alliance[i].war )
			j++; // To know if War is over
	}

	if( j == 0 )
		g->war = false; // No more enemy guilds

	for( i = 0; i < g->max_member; i++ )
	{
		if( (pl_sd = g->member[i].sd ) == NULL )
			continue;

		clif_disp_onlyself(pl_sd, output, len);
	}

	if( surrender ) return;

	sprintf(output, "Guild [%s] has surrender against Guild [%s]", eg->name, g->name);
	clif_broadcast(NULL, output, strlen(output) + 1, 0, ALL_CLIENT);
}

/*==========================================
 * Retrieves and validates the sd pointer for this guild member [Skotlex]
 *------------------------------------------*/
static TBL_PC* guild_sd_check(int guild_id, uint32 account_id, uint32 char_id) {
	TBL_PC* sd = map_id2sd(account_id);

	if (!(sd && sd->status.char_id == char_id))
		return NULL;

	if (sd->status.guild_id != guild_id)
	{	//If player belongs to a different guild, kick him out.
		intif_guild_leave(guild_id,account_id,char_id,0,"** Guild Mismatch **");
		return NULL;
	}

	return sd;
}

// Modified [Komurka]
int guild_skill_get_max (int id) {
	if ((id = guild_skill_get_index(id)) < 0)
		return 0;
	return guild_skill_tree[id].max;
}

// Retrieve skill_lv learned by guild
int guild_checkskill(struct guild *g, int id) {
	if ((id = guild_skill_get_index(id)) < 0)
		return 0;
	return g->skill[id].lv;
}

/*==========================================
 * guild_skill_tree.txt reading - from jA [Komurka]
 *------------------------------------------*/
static bool guild_read_guildskill_tree_db(char* split[], int columns, int current) {// <skill id>,<max lv>,<req id1>,<req lv1>,<req id2>,<req lv2>,<req id3>,<req lv3>,<req id4>,<req lv4>,<req id5>,<req lv5>
	int k, skill_id = atoi(split[0]);
	short idx = -1;

	if ((idx = guild_skill_get_index(skill_id)) < 0) {
		ShowError("guild_read_guildskill_tree_db: Invalid Guild skill '%s'.\n", split[1]);
		return false;
	}

	guild_skill_tree[idx].id = skill_id;
	guild_skill_tree[idx].max = atoi(split[1]);

	if( guild_skill_tree[idx].id == GD_GLORYGUILD && battle_config.require_glory_guild && guild_skill_tree[idx].max == 0 ) 	{// enable guild's glory when required for emblems
		guild_skill_tree[idx].max = 1;
	}

	for( k = 0; k < MAX_GUILD_SKILL_REQUIRE; k++ ) 	{
		guild_skill_tree[idx].need[k].id = atoi(split[k*2+2]);
		guild_skill_tree[idx].need[k].lv = atoi(split[k*2+3]);
	}

	return true;
}

/*==========================================
 * Guild skill check - from jA [Komurka]
 *------------------------------------------*/
int guild_check_skill_require(struct guild *g,int id) {
	uint8 i;
	short idx = -1;

	if(g == NULL)
		return 0;

	if ((idx = guild_skill_get_index(id)) < 0)
		return 0;

	for(i=0;i<MAX_GUILD_SKILL_REQUIRE;i++)
	{
		if(guild_skill_tree[idx].need[i].id == 0) break;
		if(guild_skill_tree[idx].need[i].lv > guild_checkskill(g,guild_skill_tree[idx].need[i].id))
			return 0;
	}
	return 1;
}

static bool guild_read_castledb(char* str[], int columns, int current) {// <castle id>,<map name>,<castle name>,<castle event>[,<reserved/unused switch flag>]
	struct guild_castle *gc;
	int mapindex = mapindex_name2id(str[1]);

	if (map_mapindex2mapid(mapindex) < 0) // Map not found or on another map-server
		return false;

	CREATE(gc, struct guild_castle, 1);
	gc->castle_id = atoi(str[0]);
	gc->mapindex = mapindex;
	safestrncpy(gc->castle_name, str[2], sizeof(gc->castle_name));
	safestrncpy(gc->castle_event, str[3], sizeof(gc->castle_event));

	idb_put(castle_db,gc->castle_id,gc);
	return true;
}

/// lookup: guild id -> guild*
struct guild* guild_search(int guild_id) {
	return (struct guild*)idb_get(guild_db,guild_id);
}

/// lookup: guild name -> guild*
struct guild* guild_searchname(char* str) {
	struct guild* g;
	DBIterator *iter = db_iterator(guild_db);

	for( g = (struct guild*)dbi_first(iter); dbi_exists(iter); g = (struct guild*)dbi_next(iter) ) {
		if( strcmpi(g->name, str) == 0 )
			break;
	}
	dbi_destroy(iter);

	return g;
}

/// lookup: castle id -> castle*
struct guild_castle* guild_castle_search(int gcid) {
	return (struct guild_castle*)idb_get(castle_db,gcid);
}

/// lookup: map index -> castle*
struct guild_castle* guild_mapindex2gc(short mapindex) {
	struct guild_castle* gc;
	DBIterator *iter = db_iterator(castle_db);

	for( gc = (struct guild_castle*)dbi_first(iter); dbi_exists(iter); gc = (struct guild_castle*)dbi_next(iter) ) {
		if( gc->mapindex == mapindex )
			break;
	}
	dbi_destroy(iter);

	return gc;
}

/// lookup: map name -> castle*
struct guild_castle* guild_mapname2gc(const char* mapname) {
	return guild_mapindex2gc(mapindex_name2id(mapname));
}

struct map_session_data* guild_getavailablesd(struct guild* g) {
	int i;

	nullpo_retr(NULL, g);

	ARR_FIND( 0, g->max_member, i, g->member[i].sd != NULL );
	return( i < g->max_member ) ? g->member[i].sd : NULL;
}

/// lookup: player AID/CID -> member index
int guild_getindex(struct guild *g,uint32 account_id,uint32 char_id) {
	int i;

	if( g == NULL )
		return -1;

	ARR_FIND( 0, g->max_member, i, g->member[i].account_id == account_id && g->member[i].char_id == char_id );
	return( i < g->max_member ) ? i : -1;
}

/// lookup: player sd -> member position
int guild_getposition(struct guild* g, struct map_session_data* sd) {
	int i;

	if( g == NULL && (g=sd->guild) == NULL )
		return -1;

	ARR_FIND( 0, g->max_member, i, g->member[i].account_id == sd->status.account_id && g->member[i].char_id == sd->status.char_id );
	return( i < g->max_member ) ? g->member[i].position : -1;
}

//Creation of member information
void guild_makemember(struct guild_member *m,struct map_session_data *sd) {
	nullpo_retv(sd);

	memset(m,0,sizeof(struct guild_member));
	m->account_id	= sd->status.account_id;
	m->char_id		= sd->status.char_id;
	m->hair			= sd->status.hair;
	m->hair_color	= sd->status.hair_color;
	m->gender		= sd->status.sex;
	m->class_		= sd->status.class_;
	m->lv			= sd->status.base_level;
//	m->exp			= 0;
//	m->exp_payper	= 0;
	m->online		= 1;
	m->position		= MAX_GUILDPOSITION-1;
	memcpy(m->name,sd->status.name,NAME_LENGTH);
	return;
}

/**
 * Server cache to be flushed to inter the Guild EXP
 * @see DBApply
 */
int guild_payexp_timer_sub(DBKey key, DBData *data, va_list ap) {
	int i;
	struct guild_expcache *c;
	struct guild *g;

	c = (struct guild_expcache *)db_data2ptr(data);

	if (
		(g = guild_search(c->guild_id)) == NULL ||
		(i = guild_getindex(g, c->account_id, c->char_id)) < 0
	) {
		ers_free(expcache_ers, c);
		return 0;
	}

	if (g->member[i].exp > UINT64_MAX - c->exp)
		g->member[i].exp = UINT64_MAX;
	else
		g->member[i].exp+= c->exp;

	intif_guild_change_memberinfo(g->guild_id,c->account_id,c->char_id,
		GMI_EXP,&g->member[i].exp,sizeof(g->member[i].exp));
	c->exp=0;

	ers_free(expcache_ers, c);
	return 0;
}

int guild_payexp_timer(int tid, unsigned int tick, int id, intptr_t data) {
	guild_expcache_db->clear(guild_expcache_db,guild_payexp_timer_sub);
	return 0;
}

/**
 * Taken from party_send_xy_timer_sub. [Skotlex]
 * @see DBApply
 */
int guild_send_xy_timer_sub(DBKey key, DBData *data, va_list ap) {
	struct guild *g = (struct guild *)db_data2ptr(data);
	int i;

	nullpo_ret(g);

	if( !g->connect_member ) {
		// no members connected to this guild so do not iterate
		return 0;
	}

	for(i=0;i<g->max_member;i++){
		struct map_session_data* sd = g->member[i].sd;
		if( sd != NULL && sd->fd && (sd->guild_x != sd->bl.x || sd->guild_y != sd->bl.y) && !sd->bg_id ) {
			clif_guild_xy(sd);
			sd->guild_x = sd->bl.x;
			sd->guild_y = sd->bl.y;
		}
	}
	return 0;
}

//Code from party_send_xy_timer [Skotlex]
static int guild_send_xy_timer(int tid, unsigned int tick, int id, intptr_t data) {
	guild_db->foreach(guild_db,guild_send_xy_timer_sub,tick);
	return 0;
}

int guild_send_dot_remove(struct map_session_data *sd) {
	if (sd->status.guild_id)
		clif_guild_xy_remove(sd);
	return 0;
}
//------------------------------------------------------------------------

int guild_create(struct map_session_data *sd, const char *name) {
	char tname[NAME_LENGTH];
	struct guild_member m;
	nullpo_ret(sd);

	safestrncpy(tname, name, NAME_LENGTH);
	trim(tname);

	if( !tname[0] )
		return 0; // empty name

	if( sd->status.guild_id ) {
		// already in a guild
		clif_guild_created(sd,1);
		return 0;
	}
	if( battle_config.guild_emperium_check && pc_search_inventory(sd,ITEMID_EMPERIUM) == -1 ) {
		// item required
		clif_guild_created(sd,3);
		return 0;
	}

	guild_makemember(&m,sd);
	m.position=0;
	intif_guild_create(name,&m);
	return 1;
}

//Whether or not to create guild
int guild_created(uint32 account_id,int guild_id) {
	struct map_session_data *sd=map_id2sd(account_id);

	if(sd==NULL)
		return 0;
	if(!guild_id) {
		clif_guild_created(sd, 2); // Creation failure (presence of the same name Guild)
		return 0;
	}

	sd->status.guild_id = guild_id;
	clif_guild_created(sd,0);
	if(battle_config.guild_emperium_check){
		int index = pc_search_inventory(sd,ITEMID_EMPERIUM);

		if( index > 0 )
			pc_delitem(sd,index,1,0,0,LOG_TYPE_CONSUME);	//emperium consumption
	}
	return 0;
}

//Information request
int guild_request_info(int guild_id) {
	return intif_guild_request_info(guild_id);
}

//Information request with event
int guild_npc_request_info(int guild_id,const char *event) {
	if( guild_search(guild_id) ) {
		if( event && *event )
			npc_event_do(event);

		return 0;
	}

	if( event && *event ) {
		struct eventlist *ev;
		DBData prev;
		ev=(struct eventlist *)aCalloc(sizeof(struct eventlist),1);
		memcpy(ev->name,event,strlen(event));
		//The one in the db (if present) becomes the next event from this.
		if (guild_infoevent_db->put(guild_infoevent_db, db_i2key(guild_id), db_ptr2data(ev), &prev))
			ev->next = (struct eventlist *)db_data2ptr(&prev);
	}

	return guild_request_info(guild_id);
}

/**
 * Close trade window if party member is kicked when trade a party bound item
 * @param sd
 **/
static void guild_trade_bound_cancel(struct map_session_data *sd) {
#ifdef BOUND_ITEMS
	nullpo_retv(sd);
	if (sd->state.isBoundTrading&(1<<BOUND_GUILD))
		trade_tradecancel(sd);
#else
	;
#endif
}

//Confirmation of the character belongs to guild
int guild_check_member(struct guild *g) {
	int i;
	struct map_session_data *sd;
	struct s_mapiterator* iter;

	nullpo_ret(g);

	iter = mapit_getallusers();
	for( sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter) ) {
		if( sd->status.guild_id != g->guild_id )
			continue;

		i = guild_getindex(g,sd->status.account_id,sd->status.char_id);
		if (i < 0) {
			sd->guild = NULL;
			sd->status.guild_id=0;
			sd->guild_emblem_id=0;
			ShowWarning("guild: check_member %d[%s] is not member\n",sd->status.account_id,sd->status.name);
			status_calc_pc(sd,0); // Regional System
		}
	}
	mapit_free(iter);

	return 0;
}

//Delete association with guild_id for all characters
int guild_recv_noinfo(int guild_id) {
	struct map_session_data *sd;
	struct s_mapiterator* iter;

	iter = mapit_getallusers();
	for( sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); sd = (TBL_PC*)mapit_next(iter) ) {
		if( sd->status.guild_id == guild_id )
		{
			sd->status.guild_id = 0; // erase guild
			status_calc_pc(sd,0); // Regional System
		}
	}
	mapit_free(iter);

	return 0;
}

//Get and display information for all member
int guild_recv_info(struct guild *sg) {
	struct guild *g,before;
	int i,bm,m;
	DBData data;
	struct map_session_data *sd;
	bool guild_new = false;

	nullpo_ret(sg);

	if((g = guild_search(sg->guild_id))==NULL) {
		guild_new = true;
		g=(struct guild *)aCalloc(1,sizeof(struct guild));
		idb_put(guild_db,sg->guild_id,g);
		before=*sg;
		//Perform the check on the user because the first load
		guild_check_member(sg);
		if ((sd = map_nick2sd(sg->master)) != NULL) {
			sd->guild = g;
			sd->state.gmaster_flag = 1;
			clif_charnameupdate(sd); // [LuzZza]
			clif_guild_masterormember(sd);
		}
	} else {
		before=*g;
	}
	memcpy(g,sg,sizeof(struct guild));
	for( i = 0; i < MAX_GUILDSKILL; i++ )
	{
		if( guild_new )
			g->skill_block_timer[i] = INVALID_TIMER;
		else
			g->skill_block_timer[i] = before.skill_block_timer[i];
	}

	if(g->max_member > MAX_GUILD) {
		ShowError("guild_recv_info: Received guild with %d members, but MAX_GUILD is only %d. Extra guild-members have been lost!\n", g->max_member, MAX_GUILD);
		g->max_member = MAX_GUILD;
	}

	for(i=bm=m=0;i<g->max_member;i++){
		if(g->member[i].account_id>0){
			sd = g->member[i].sd = guild_sd_check(g->guild_id, g->member[i].account_id, g->member[i].char_id);
			if (sd) clif_charnameupdate(sd); // [LuzZza]
			m++;
		}else
			g->member[i].sd=NULL;
		if(before.member[i].account_id>0)
			bm++;
	}

	for (i = 0; i < g->max_member; i++) { //Transmission of information at all members
		sd = g->member[i].sd;
		if( sd==NULL )
			continue;
		sd->guild = g;
		if(channel_config.ally_autojoin ) {
			channel_gjoin(sd,3); //make all member join guildchan+allieschan
		}

		if (before.guild_lv != g->guild_lv || bm != m ||
			before.max_member != g->max_member) {
			clif_guild_basicinfo(sd); //Submit basic information
			clif_guild_emblem(sd, g); //Submit emblem
		}

		if (bm != m) { //Send members information
			clif_guild_memberlist(g->member[i].sd);
		}

		if (before.skill_point != g->skill_point)
			clif_guild_skillinfo(sd); //Submit information skills

		if (guild_new) { // Send information and affiliation if unsent
			clif_guild_belonginfo(sd, g);
			clif_guild_notice(sd, g);
			sd->guild_emblem_id = g->emblem_id;
		}
		if (g->instance_id != 0)
			instance_reqinfo(sd, g->instance_id);
	}

	if( battle_config.guild_wars )
	{
		struct guild *eg; // To hold Enemy Data
		int j;

		for( i = 0; i < MAX_GUILDALLIANCE; i++ )
		{
			if( !g->alliance[i].guild_id || g->alliance[i].opposition == 0 )
				continue;

			if( (eg = idb_get(guild_db, g->alliance[i].guild_id)) == NULL )
				continue; // This guild is not loaded

			ARR_FIND(0, MAX_GUILDALLIANCE, j, eg->alliance[j].guild_id == g->guild_id && eg->alliance[j].opposition != 0 );
			if( j < MAX_GUILDALLIANCE )
			{ // Double Opposition = War
				g->alliance[i].war = eg->alliance[i].war = true;
				guild_war_init(g, eg, guild_new);
				guild_war_init(eg, g, guild_new);
			}
		}
	}

	//Occurrence of an event
	if (guild_infoevent_db->remove(guild_infoevent_db, db_i2key(sg->guild_id), &data)) {
		struct eventlist *ev = (struct eventlist *)db_data2ptr(&data), *ev2;
		while(ev) {
			npc_event_do(ev->name);
			ev2=ev->next;
			aFree(ev);
			ev=ev2;
		}
	}

	return 0;
}

/*=============================================
 * Player sd send a guild invatation to player tsd to join his guild
 *--------------------------------------------*/
int guild_invite(struct map_session_data *sd, struct map_session_data *tsd) {
	struct guild *g;
	int i;

	nullpo_ret(sd);

	g=sd->guild;

	if(tsd==NULL || g==NULL)
		return 0;

	if( !battle_config.faction_allow_guild && sd->status.faction_id != tsd->status.faction_id )
	{
		clif_displaymessage(sd->fd,"You cannot invite to guild other faction's members.");
		return 0;
	}

	if( (i=guild_getposition(g,sd))<0 || !(g->position[i].mode&0x0001) )
		return 0; //Invite permission.

	if(!battle_config.invite_request_check) {
	if (tsd->party_invite > 0 || tsd->trade_partner || tsd->adopt_invite) { //checking if there no other invitation pending
			clif_guild_inviteack(sd,0);
			return 0;
		}
	}

	if (!tsd->fd) { //You can't invite someone who has already disconnected.
		clif_guild_inviteack(sd,1);
		return 0;
	}

	if(tsd->status.guild_id>0 ||
		tsd->guild_invite>0 ||
		((agit_flag || agit2_flag) && map[tsd->bl.m].flag.gvg_castle))
	{	//Can't invite people inside castles. [Skotlex]
		clif_guild_inviteack(sd,0);
		return 0;
	}

	//search an empty spot in guild
	ARR_FIND( 0, g->max_member, i, g->member[i].account_id == 0 );
	if(i==g->max_member){
		clif_guild_inviteack(sd,3);
		return 0;
	}

	tsd->guild_invite=sd->status.guild_id;
	tsd->guild_invite_account=sd->status.account_id;

	clif_guild_invite(tsd,g);
	return 0;
}

/// Guild invitation reply.
/// flag: 0:rejected, 1:accepted
int guild_reply_invite(struct map_session_data* sd, int guild_id, int flag) {
	struct map_session_data* tsd;

	nullpo_ret(sd);

	// subsequent requests may override the value
	if( sd->guild_invite != guild_id )
		return 0; // mismatch

	// look up the person who sent the invite
	//NOTE: this can be NULL because the person might have logged off in the meantime
	tsd = map_id2sd(sd->guild_invite_account);

	if ( sd->status.guild_id > 0 ) {
	// [Paradox924X]
	 // Already in another guild.
		if ( tsd ) clif_guild_inviteack(tsd,0);
		return 0;
	} else if( flag == 0 ) {// rejected
		sd->guild_invite = 0;
		sd->guild_invite_account = 0;
		if( tsd ) clif_guild_inviteack(tsd,1);
	} else {// accepted
		struct guild_member m;
		struct guild* g;
		int i;

		if( (g=guild_search(guild_id)) == NULL ) {
			sd->guild_invite = 0;
			sd->guild_invite_account = 0;
			return 0;
		}

		ARR_FIND( 0, g->max_member, i, g->member[i].account_id == 0 );
		if( i == g->max_member ) {
			sd->guild_invite = 0;
			sd->guild_invite_account = 0;
			if( tsd ) clif_guild_inviteack(tsd,3);
			return 0;
		}

		guild_makemember(&m,sd);
		intif_guild_addmember(guild_id, &m);
		//TODO: send a minimap update to this player
	}

	return 0;
}

//Invoked when a player joins.
//- If guild is not in memory, it is requested
//- Otherwise sd pointer is set up.
//- Player must be authed and must belong to a guild before invoking this method
void guild_member_joined(struct map_session_data *sd, bool calc) {
	struct guild* g;
	int i;
	g=guild_search(sd->status.guild_id);
	if (!g) {
		guild_request_info(sd->status.guild_id);
		return;
	}
	if (strcmp(sd->status.name,g->master) == 0) {	// set the Guild Master flag
		sd->state.gmaster_flag = 1;
		// prevent Guild Skills from being used directly after relog
	}
	i = guild_getindex(g, sd->status.account_id, sd->status.char_id);
	if (i == -1)
	{
		sd->status.guild_id = 0;
		if( calc ) status_calc_pc(sd,0); // Regional System
	}
	else {
		g->member[i].sd = sd;
		sd->guild = g;

		if (g->instance_id != 0)
			instance_reqinfo(sd, g->instance_id);
		if( channel_config.ally_enable && channel_config.ally_autojoin ) {
			channel_gjoin(sd,3);
		}
	}
}

/*==========================================
 * Add a player to a given guild_id
 *----------------------------------------*/
int guild_member_added(int guild_id,uint32 account_id,uint32 char_id,int flag) {
	struct map_session_data *sd= map_id2sd(account_id),*sd2;
	struct guild *g;

	if( (g=guild_search(guild_id))==NULL )
		return 0;

	if(sd==NULL || sd->guild_invite==0){
	// cancel if player not present or invalide guild_id invitation
		if (flag == 0) {
			ShowError("guild: member added error %d is not online\n",account_id);
 			intif_guild_leave(guild_id,account_id,char_id,0,"** Data Error **");
		}
		return 0;
	}
	sd2 = map_id2sd(sd->guild_invite_account);
	sd->guild_invite = 0;
	sd->guild_invite_account = 0;

	if (flag == 1) { //failure
		if( sd2!=NULL )
			clif_guild_inviteack(sd2,3);
		return 0;
	}

	//if all ok add player to guild
	sd->status.guild_id = g->guild_id;
	sd->guild_emblem_id = g->emblem_id;
	sd->guild = g;
	//Packets which were sent in the previous 'guild_sent' implementation.
	clif_guild_belonginfo(sd,g);
	clif_guild_notice(sd,g);
	status_calc_pc(sd,0); // Regional System

	//TODO: send new emblem info to others

	if( sd2!=NULL )
		clif_guild_inviteack(sd2,2);

	//Next line commented because it do nothing, look at guild_recv_info [LuzZza]
	//clif_charnameupdate(sd); //Update display name [Skotlex]

	if (g->instance_id != 0)
		instance_reqinfo(sd, g->instance_id);

	return 0;
}

/*==========================================
 * Player request leaving a given guild_id
 *----------------------------------------*/
int guild_leave(struct map_session_data* sd, int guild_id, uint32 account_id, uint32 char_id, const char* mes) {
	struct guild *g;

	nullpo_ret(sd);

	g = sd->guild;

	if(g==NULL)
		return 0;

	if(sd->status.account_id!=account_id ||
		sd->status.char_id!=char_id || sd->status.guild_id!=guild_id ||
		((agit_flag || agit2_flag) && map[sd->bl.m].flag.gvg_castle))
		return 0;

	guild_trade_bound_cancel(sd);
	intif_guild_leave(sd->status.guild_id, sd->status.account_id, sd->status.char_id,0,mes);
	return 0;
}

/*==========================================
 * Request remove a player to a given guild_id
 *----------------------------------------*/
int guild_expulsion(struct map_session_data* sd, int guild_id, uint32 account_id, uint32 char_id, const char* mes) {
	struct map_session_data *tsd;
	struct guild *g;
	int i,ps;

	nullpo_ret(sd);

	g = sd->guild;

	if(g==NULL)
		return 0;

	if(sd->status.guild_id!=guild_id)
		return 0;

	if( (ps=guild_getposition(g,sd))<0 || !(g->position[ps].mode&0x0010) )
		return 0;	//Expulsion permission

	//Can't leave inside guild castles.
	if ((tsd = map_id2sd(account_id)) &&
		tsd->status.char_id == char_id &&
		((agit_flag || agit2_flag) && map[tsd->bl.m].flag.gvg_castle))
		return 0;

	// find the member and perform expulsion
	i = guild_getindex(g, account_id, char_id);
	if( i != -1 && strcmp(g->member[i].name,g->master) != 0 ) { //Can't expel the GL!
		if (tsd)
			guild_trade_bound_cancel(tsd);
		intif_guild_leave(g->guild_id,account_id,char_id,1,mes);
	}

	return 0;
}

/**
* A confirmation from inter-serv that player is kicked successfully
* @param guild_Id
* @param account_id
* @param char_id
* @param flag
* @param name
* @param mes
*/
int guild_member_withdraw(int guild_id, uint32 account_id, uint32 char_id, int flag, const char* name, const char* mes) {
	int i;
	struct guild* g = guild_search(guild_id);
	struct map_session_data* sd = map_charid2sd(char_id);
	struct map_session_data* online_member_sd;

	if(g == NULL)
		return 0; // no such guild (error!)

	i = guild_getindex(g, account_id, char_id);
	if( i == -1 )
		return 0; // not a member (inconsistency!)

#ifdef BOUND_ITEMS
	//Guild bound item check
	guild_retrieveitembound(char_id,account_id,guild_id);
#endif

	online_member_sd = guild_getavailablesd(g);
	if(online_member_sd == NULL)
		return 0; // noone online to inform


	if(!flag)
		clif_guild_leave(online_member_sd, name, mes);
	else
		clif_guild_expulsion(online_member_sd, name, mes, account_id);

	// remove member from guild
	memset(&g->member[i],0,sizeof(struct guild_member));
	clif_guild_memberlist(online_member_sd);

	// update char, if online
	if(sd != NULL && sd->status.guild_id == guild_id) {
		// do stuff that needs the guild_id first, BEFORE we wipe it
		if (sd->state.storage_flag == 2) //Close the guild storage.
			gstorage_storageclose(sd);
		guild_send_dot_remove(sd);
		channel_pcquit(sd,3); //leave guild and ally chan
		sd->status.guild_id = 0;
		sd->guild = NULL;
		sd->guild_emblem_id = 0;

		if (g->instance_id) {
			int16 m = sd->bl.m;

			if (map[m].instance_id) { // User was on the instance map
				if (map[m].save.map)
					pc_setpos(sd, map[m].save.map, map[m].save.x, map[m].save.y, CLR_TELEPORT);
				else
					pc_setpos(sd, sd->status.save_point.map, sd->status.save_point.x, sd->status.save_point.y, CLR_TELEPORT);
			}
		}

		clif_charnameupdate(sd); //Update display name [Skotlex]
		status_change_end(&sd->bl,SC_LEADERSHIP,INVALID_TIMER);
		status_change_end(&sd->bl,SC_GLORYWOUNDS,INVALID_TIMER);
		status_change_end(&sd->bl,SC_SOULCOLD,INVALID_TIMER);
		status_change_end(&sd->bl,SC_HAWKEYES,INVALID_TIMER);
		//@TODO: Send emblem update to self and people around
		status_calc_pc(sd,SCO_NONE); // Regional System
	}
	return 0;
}

#ifdef BOUND_ITEMS
/**
* Retrieve guild bound items from kicked member
* @param char_id
* @param account_id
* @param guild_id
*/
void guild_retrieveitembound(uint32 char_id, uint32 account_id, int guild_id) {
	TBL_PC *sd = map_charid2sd(char_id);
	if (sd) { //Character is online
		int idxlist[MAX_INVENTORY];
		int j;
		j = pc_bound_chk(sd,BOUND_GUILD,idxlist);
		if (j) {
			struct guild_storage* stor = gstorage_guild2storage(sd->status.guild_id);
			int i;
			// Close the storage first if someone open it
			if (stor && stor->opened) {
				struct map_session_data *tsd = map_charid2sd(stor->opened);
				if (tsd)
					gstorage_storageclose(tsd);
			}
			for (i = 0; i < j; i++) { //Loop the matching items, gstorage_additem takes care of opening storage
				if (stor)
					gstorage_additem(sd,stor,&sd->status.inventory[idxlist[i]],sd->status.inventory[idxlist[i]].amount);
				pc_delitem(sd,idxlist[i],sd->status.inventory[idxlist[i]].amount,0,4,LOG_TYPE_GSTORAGE);
			}
			gstorage_storageclose(sd); //Close and save the storage
		}
	} else { //Character is offline, ask char server to do the job
		struct guild_storage* stor = gstorage_get_storage(guild_id);
		struct guild *g = guild_search(guild_id);
		nullpo_retv(g);
		if(stor && stor->opened) { //Someone is in guild storage, close them
			struct map_session_data *tsd = map_charid2sd(stor->opened);
			if (tsd)
				gstorage_storageclose(tsd);
		}
		intif_itembound_guild_retrieve(char_id,account_id,guild_id);
	}
}
#endif

int guild_send_memberinfoshort(struct map_session_data *sd,int online) { // cleaned up [LuzZza]
	struct guild *g;

	nullpo_ret(sd);

	if(sd->status.guild_id <= 0)
		return 0;

	if(!(g = sd->guild))
		return 0;

	intif_guild_memberinfoshort(g->guild_id,
		sd->status.account_id,sd->status.char_id,online,sd->status.base_level,sd->status.class_);

	if(!online){
		int i=guild_getindex(g,sd->status.account_id,sd->status.char_id);
		if(i>=0)
			g->member[i].sd=NULL;
		else
			ShowError("guild_send_memberinfoshort: Failed to locate member %d:%d in guild %d!\n", sd->status.account_id, sd->status.char_id, g->guild_id);
		return 0;
	}

	if(sd->state.connect_new) {	//Note that this works because it is invoked in parse_LoadEndAck before connect_new is cleared.
		clif_guild_belonginfo(sd,g);
		sd->guild_emblem_id = g->emblem_id;
	}
	return 0;
}

int guild_recv_memberinfoshort(int guild_id,uint32 account_id,uint32 char_id,int online,int lv,int class_) { // cleaned up [LuzZza]

	int i,alv,c,idx=-1,om=0,oldonline=-1;
	struct guild *g = guild_search(guild_id);

	if(g == NULL)
		return 0;

	for(i=0,alv=0,c=0,om=0;i<g->max_member;i++){
		struct guild_member *m=&g->member[i];
		if(!m->account_id) continue;
		if(m->account_id==account_id && m->char_id==char_id ){
			oldonline=m->online;
			m->online=online;
			m->lv=lv;
			m->class_=class_;
			idx=i;
		}
		alv+=m->lv;
		c++;
		if(m->online)
			om++;
	}

	if(idx == -1 || c == 0) {
        //Treat char_id who doesn't match guild_id (not found as member)
		struct map_session_data *sd = map_id2sd(account_id);
		if(sd && sd->status.char_id == char_id) {
			sd->status.guild_id=0;
			sd->guild_emblem_id=0;
			status_calc_pc(sd,0); // Regional System
		}
		ShowWarning("guild: not found member %d,%d on %d[%s]\n",	account_id,char_id,guild_id,g->name);
		return 0;
	}

	g->average_lv=alv/c;
	g->connect_member=om;

	//Ensure validity of pointer (ie: player logs in/out, changes map-server)
	g->member[idx].sd = guild_sd_check(guild_id, account_id, char_id);

	if(oldonline!=online)
		clif_guild_memberlogin_notice(g, idx, online);

	if(!g->member[idx].sd)
		return 0;

	//Send XY dot updates. [Skotlex]
	//Moved from guild_send_memberinfoshort [LuzZza]
	for(i=0; i < g->max_member; i++) {

		if(!g->member[i].sd || i == idx ||
			g->member[i].sd->bl.m != g->member[idx].sd->bl.m)
			continue;

		clif_guild_xy_single(g->member[idx].sd->fd, g->member[i].sd);
		clif_guild_xy_single(g->member[i].sd->fd, g->member[idx].sd);
	}

	return 0;
}

/*====================================================
 * Send a message to whole guild
 *---------------------------------------------------*/
int guild_send_message(struct map_session_data *sd,const char *mes,int len) {
	nullpo_ret(sd);

	if(sd->status.guild_id==0)
		return 0;
	intif_guild_message(sd->status.guild_id,sd->status.account_id,mes,len);
	guild_recv_message(sd->status.guild_id,sd->status.account_id,mes,len);

	// Chat logging type 'G' / Guild Chat
	log_chat(LOG_CHAT_GUILD, sd->status.guild_id, sd->status.char_id, sd->status.account_id, mapindex_id2name(sd->mapindex), sd->bl.x, sd->bl.y, NULL, mes);

	return 0;
}

/*====================================================
 * Guild receive a message, will be displayed to whole member
 *---------------------------------------------------*/
int guild_recv_message(int guild_id,uint32 account_id,const char *mes,int len) {
	struct guild *g;
	if( (g=guild_search(guild_id))==NULL)
		return 0;
	clif_guild_message(g,account_id,mes,len);
	return 0;
}

/*====================================================
 * Member changing position in guild
 *---------------------------------------------------*/
int guild_change_memberposition(int guild_id,uint32 account_id,uint32 char_id,short idx) {
	return intif_guild_change_memberinfo(guild_id,account_id,char_id,GMI_POSITION,&idx,sizeof(idx));
}

/*====================================================
 * Notification of new position for member
 *---------------------------------------------------*/
int guild_memberposition_changed(struct guild *g,int idx,int pos) {
	nullpo_ret(g);

	g->member[idx].position=pos;
	clif_guild_memberpositionchanged(g,idx);

	// Update char position in client [LuzZza]
	if(g->member[idx].sd != NULL)
		clif_charnameupdate(g->member[idx].sd);
	return 0;
}

/*====================================================
 * Change guild title or member
 *---------------------------------------------------*/
int guild_change_position(int guild_id,int idx,
	int mode,int exp_mode,const char *name) {
	struct guild_position p;

	exp_mode = cap_value(exp_mode, 0, battle_config.guild_exp_limit);
	//Mode 0x01 <- Invite
	//Mode 0x10 <- Expel.
	p.mode=mode&0x11;
	p.exp_mode=exp_mode;
	safestrncpy(p.name,name,NAME_LENGTH);
	return intif_guild_position(guild_id,idx,&p);
}

/*====================================================
 * Notification of member has changed his guild title
 *---------------------------------------------------*/
int guild_position_changed(int guild_id,int idx,struct guild_position *p) {
	struct guild *g=guild_search(guild_id);
	int i;
	if(g==NULL)
		return 0;
	memcpy(&g->position[idx],p,sizeof(struct guild_position));
	clif_guild_positionchanged(g,idx);

	// Update char name in client [LuzZza]
	for(i=0;i<g->max_member;i++)
		if(g->member[i].position == idx && g->member[i].sd != NULL)
			clif_charnameupdate(g->member[i].sd);
	return 0;
}

/*====================================================
 * Change guild notice
 *---------------------------------------------------*/
int guild_change_notice(struct map_session_data *sd,int guild_id,const char *mes1,const char *mes2) {
	nullpo_ret(sd);

	if(guild_id!=sd->status.guild_id)
		return 0;
	return intif_guild_notice(guild_id,mes1,mes2);
}

/*====================================================
 * Notification of guild has changed his notice
 *---------------------------------------------------*/
int guild_notice_changed(int guild_id,const char *mes1,const char *mes2) {
	int i;
	struct guild *g=guild_search(guild_id);
	if(g==NULL)
		return 0;

	memcpy(g->mes1,mes1,MAX_GUILDMES1);
	memcpy(g->mes2,mes2,MAX_GUILDMES2);

	for(i=0;i<g->max_member;i++){
		struct map_session_data *sd = g->member[i].sd;
		if(sd != NULL)
			clif_guild_notice(sd,g);
	}
	return 0;
}

/*====================================================
 * Change guild emblem
 *---------------------------------------------------*/
int guild_change_emblem(struct map_session_data *sd,int len,const char *data) {
	struct guild *g;
	nullpo_ret(sd);

	if (battle_config.require_glory_guild &&
		!((g = sd->guild) && guild_checkskill(g, GD_GLORYGUILD)>0)) {
		clif_skill_fail(sd,GD_GLORYGUILD,USESKILL_FAIL_LEVEL,0);
		return 0;
	}

	return intif_guild_emblem(sd->status.guild_id,len,data);
}

/*====================================================
 * Notification of guild emblem changed
 *---------------------------------------------------*/
int guild_emblem_changed(int len,int guild_id,int emblem_id,const char *data) {
	int i;
	struct map_session_data *sd;
	struct guild *g=guild_search(guild_id);
	if(g==NULL)
		return 0;

	memcpy(g->emblem_data,data,len);
	g->emblem_len=len;
	g->emblem_id=emblem_id;

	for(i=0;i<g->max_member;i++){
		if((sd=g->member[i].sd)!=NULL){
			sd->guild_emblem_id=emblem_id;
			clif_guild_belonginfo(sd,g);
			clif_guild_emblem(sd,g);
			clif_guild_emblem_area(&sd->bl);
		}
	}
	{// update guardians (mobs)
		DBIterator* iter = db_iterator(castle_db);
		struct guild_castle* gc;
		for( gc = (struct guild_castle*)dbi_first(iter) ; dbi_exists(iter); gc = (struct guild_castle*)dbi_next(iter) )
		{
			if( gc->guild_id != guild_id )
				continue;
			// update permanent guardians
			for( i = 0; i < ARRAYLENGTH(gc->guardian); ++i )
			{
				TBL_MOB* md = (gc->guardian[i].id ? map_id2md(gc->guardian[i].id) : NULL);
				if( md == NULL || md->guardian_data == NULL )
					continue;
				md->guardian_data->emblem_id = emblem_id;
				clif_guild_emblem_area(&md->bl);
			}
			// update temporary guardians
			for( i = 0; i < gc->temp_guardians_max; ++i )
			{
				TBL_MOB* md = (gc->temp_guardians[i] ? map_id2md(gc->temp_guardians[i]) : NULL);
				if( md == NULL || md->guardian_data == NULL )
					continue;
				md->guardian_data->emblem_id = emblem_id;
				clif_guild_emblem_area(&md->bl);
			}
		}
		dbi_destroy(iter);
	}
	{// update npcs (flags or other npcs that used flagemblem to attach to this guild)
		for( i = 0; i < guild_flags_count; i++ ) {
			if( guild_flags[i] && guild_flags[i]->u.scr.guild_id == guild_id ) {
				clif_guild_emblem_area(&guild_flags[i]->bl);
			}
		}
	}
	return 0;
}

/**
 * @see DBCreateData
 */
static DBData create_expcache(DBKey key, va_list args) {
	struct guild_expcache *c;
	struct map_session_data *sd = va_arg(args, struct map_session_data*);

	c = ers_alloc(expcache_ers, struct guild_expcache);
	c->guild_id = sd->status.guild_id;
	c->account_id = sd->status.account_id;
	c->char_id = sd->status.char_id;
	c->exp = 0;
	return db_ptr2data(c);
}

/*====================================================
 * Return taxed experience from player sd to guild
 *---------------------------------------------------*/
unsigned int guild_payexp(struct map_session_data *sd,unsigned int exp) {
	struct guild *g;
	struct guild_expcache *c;
	int per;

	nullpo_ret(sd);

	if (!exp) return 0;

	if (sd->status.guild_id == 0 ||
		(g = sd->guild) == NULL ||
		(per = guild_getposition(g,sd)) < 0 ||
		(per = g->position[per].exp_mode) < 1)
		return 0;


	if (per < 100)
		exp = exp * per / 100;
	//Otherwise tax everything.

	c = (struct guild_expcache *)db_data2ptr(guild_expcache_db->ensure(guild_expcache_db, db_i2key(sd->status.char_id), create_expcache, sd));

	if (c->exp > UINT64_MAX - exp)
		c->exp = UINT64_MAX;
	else
		c->exp += exp;

	return exp;
}

/*====================================================
 * Player sd pay a tribute experience to his guild
 * Add this experience to guild exp
 * [Celest]
 *---------------------------------------------------*/
int guild_getexp(struct map_session_data *sd,int exp) {
	struct guild_expcache *c;
	nullpo_ret(sd);

	if (sd->status.guild_id == 0 || sd->guild == NULL)
		return 0;

	c = (struct guild_expcache *)db_data2ptr(guild_expcache_db->ensure(guild_expcache_db, db_i2key(sd->status.char_id), create_expcache, sd));
	if (c->exp > UINT64_MAX - exp)
		c->exp = UINT64_MAX;
	else
		c->exp += exp;
	return exp;
}

// Zephyrus
int guild_score_saved(int guild_id, int index)
{
	return 0;
}

/*====================================================
 * Ask to increase guildskill skill_id
 *---------------------------------------------------*/
void guild_skillup(TBL_PC* sd, uint16 skill_id) {
	struct guild* g;
	short idx = guild_skill_get_index(skill_id);
	short max = 0;

	nullpo_retv(sd);

	if (idx == -1)
		return;

	if( sd->status.guild_id == 0 || (g=sd->guild) == NULL || // no guild
		strcmp(sd->status.name, g->master) ) // not the guild master
		return;

	max = guild_skill_get_max(skill_id);

	if( g->skill_point > 0 &&
		g->skill[idx].id != 0 &&
		g->skill[idx].lv < max )
		intif_guild_skillup(g->guild_id, skill_id, sd->status.account_id, max);
}

/*====================================================
 * Notification of guildskill skill_id increase request
 *---------------------------------------------------*/
int guild_skillupack(int guild_id,uint16 skill_id,uint32 account_id) {
	struct map_session_data *sd = map_id2sd(account_id);
	struct guild *g = guild_search(guild_id);
	int i;
	short idx = guild_skill_get_index(skill_id);

	if (g == NULL || idx == -1)
		return 0;
	if (sd != NULL) {
		int lv = g->skill[idx].lv;
		int range = skill_get_range(skill_id, lv);
		clif_skillup(sd,skill_id,lv,range,1);

		/* Guild Aura handling */
		switch( skill_id ) {
			case GD_LEADERSHIP:
			case GD_GLORYWOUNDS:
			case GD_SOULCOLD:
			case GD_HAWKEYES:
					guild_guildaura_refresh(sd,skill_id,g->skill[idx].lv);
				break;
		}
	}

	// Inform all members
	for (i = 0; i < g->max_member; i++)
		if ((sd = g->member[i].sd) != NULL)
			clif_guild_skillinfo(sd);

	return 0;
}

void guild_guildaura_refresh(struct map_session_data *sd, uint16 skill_id, uint16 skill_lv) {
	struct skill_unit_group* group = NULL;
	int type = status_skill2sc(skill_id);
	if( !(battle_config.guild_aura&((agit_flag || agit2_flag)?2:1)) &&
			!(battle_config.guild_aura&(map_flag_gvg2(sd->bl.m)?8:4)) )
		return;
	if( !skill_lv )
		return;
	if( sd->sc.data[type] && (group = skill_id2group(sd->sc.data[type]->val4)) ) {
		skill_delunitgroup(group);
		status_change_end(&sd->bl,(sc_type)type,INVALID_TIMER);
	}
	group = skill_unitsetting(&sd->bl,skill_id,skill_lv,sd->bl.x,sd->bl.y,0);
	if( group )
		sc_start4(NULL,&sd->bl,(sc_type)type,100,(battle_config.guild_aura&16)?0:skill_lv,0,0,group->group_id,600000);//duration doesn't matter these status never end with val4
	return;
}

/*====================================================
 * Count number of relations the guild has.
 * Flag:
 *	0 = allied
 *	1 = enemy
 *---------------------------------------------------*/
int guild_get_alliance_count(struct guild *g,int flag) {
	int i,c;

	nullpo_ret(g);

	for(i=c=0;i<MAX_GUILDALLIANCE;i++){
		if(	g->alliance[i].guild_id>0 &&
			g->alliance[i].opposition==flag )
			c++;
	}
	return c;
}

// Blocks all guild skills which have a common delay time.
int guild_block_skill_end(int tid, unsigned int tick, int id, intptr_t data) {
	struct guild *g;
	char output[128];
	int idx = battle_config.guild_skills_separed_delay ? (int)data - GD_SKILLBASE : 0;

	if( (g = guild_search(id)) == NULL )
		return 1;

	if( idx < 0 || idx >= MAX_GUILDSKILL )
	{
		ShowError("guild_block_skill_end invalid skill_id %d.\n", (int)data);
		return 0;
	}

	if( tid != g->skill_block_timer[idx] )
	{
		ShowError("guild_block_skill_end %d != %d.\n", g->skill_block_timer[idx], tid);
		return 0;
	}

	sprintf(output, "%s : Guild Skill %s Ready!!", g->name, skill_get_desc((int)data));
	g->skill_block_timer[idx] = INVALID_TIMER;
	clif_guild_message(g, 0, output, strlen(output));

	return 1;
}

void guild_block_skill_status(struct guild *g, int skill_id)
{
	const struct TimerData * td;
	char output[128];
	int seconds, idx;
	
	idx = battle_config.guild_skills_separed_delay ? skill_id - GD_SKILLBASE : 0;
	if( g == NULL || idx < 0 || idx >= MAX_GUILDSKILL || g->skill_block_timer[idx] == INVALID_TIMER )
		return;

	if( (td = get_timer(g->skill_block_timer[idx])) == NULL )
		return;

	seconds = DIFF_TICK(td->tick,gettick())/1000;
	sprintf(output, "%s : Cannot use guild skill %s. %d seconds remaining...", g->name, skill_get_desc(skill_id), seconds);
	clif_guild_message(g, 0, output, strlen(output));
}

void guild_block_skill_start(struct guild *g, int skill_id, int time)
{
	int idx = battle_config.guild_skills_separed_delay ? skill_id - GD_SKILLBASE : 0;
	if( g == NULL || idx < 0 || idx >= MAX_GUILDSKILL )
		return;

	if( g->skill_block_timer[idx] != INVALID_TIMER )
		delete_timer(g->skill_block_timer[idx], guild_block_skill_end);
	
	g->skill_block_timer[idx] = add_timer(gettick() + time, guild_block_skill_end, g->guild_id, skill_id);
}

/*====================================================
 * Check relation between guild_id1 and guild_id2.
 * Flag:
 *	0 = allied
 *	1 = enemy
 * Returns true if yes.
 *---------------------------------------------------*/
int guild_check_alliance(int guild_id1, int guild_id2, int flag) {
	struct guild *g;
	int i;

	g = guild_search(guild_id1);
	if (g == NULL)
		return 0;

	ARR_FIND( 0, MAX_GUILDALLIANCE, i, g->alliance[i].guild_id == guild_id2 && g->alliance[i].opposition == flag );
	return( i < MAX_GUILDALLIANCE ) ? 1 : 0;
}

/*====================================================
 * Player sd, asking player tsd an alliance between their 2 guilds
 *---------------------------------------------------*/
int guild_reqalliance(struct map_session_data *sd,struct map_session_data *tsd) {
	struct guild *g[2];
	int i;

	if(agit_flag || agit2_flag) {	// Disable alliance creation during woe [Valaris]
		clif_displaymessage(sd->fd,msg_txt(sd,676)); //"Alliances cannot be made during Guild Wars!"
		return 0;
	}	// end addition [Valaris]


	nullpo_ret(sd);

	if(tsd==NULL || tsd->status.guild_id<=0)
		return 0;

	g[0]=sd->guild;
	g[1]=tsd->guild;

	if(g[0]==NULL || g[1]==NULL)
		return 0;

	// Prevent creation alliance with same guilds [LuzZza]
	if(sd->status.guild_id == tsd->status.guild_id)
		return 0;

	if( guild_get_alliance_count(g[0],0) >= battle_config.max_guild_alliance ) {
		clif_guild_allianceack(sd,4);
		return 0;
	}
	if( guild_get_alliance_count(g[1],0) >= battle_config.max_guild_alliance ) {
		clif_guild_allianceack(sd,3);
		return 0;
	}

	if( tsd->guild_alliance>0 ){
		clif_guild_allianceack(sd,1);
		return 0;
	}

    for (i = 0; i < MAX_GUILDALLIANCE; i++) { // check if already allied
		if(	g[0]->alliance[i].guild_id==tsd->status.guild_id &&
			g[0]->alliance[i].opposition==0){
			clif_guild_allianceack(sd,0);
			return 0;
		}
	}

	tsd->guild_alliance=sd->status.guild_id;
	tsd->guild_alliance_account=sd->status.account_id;

	clif_guild_reqalliance(tsd,sd->status.account_id,g[0]->name);
	return 0;
}

/*====================================================
 * Player sd, answer to player tsd (account_id) for an alliance request
 *---------------------------------------------------*/
int guild_reply_reqalliance(struct map_session_data *sd,uint32 account_id,int flag) {
	struct map_session_data *tsd;

	nullpo_ret(sd);
	tsd= map_id2sd( account_id );
	if (!tsd) { //Character left? Cancel alliance.
		clif_guild_allianceack(sd,3);
		return 0;
	}

	if (sd->guild_alliance != tsd->status.guild_id) // proposed guild_id alliance doesn't match tsd guildid
		return 0;

	if (flag == 1) { // consent
		int i;

	struct guild *g, *tg; // Reconfirm the number of alliance
		g=sd->guild;
		tg=tsd->guild;

		if(g==NULL || guild_get_alliance_count(g,0) >= battle_config.max_guild_alliance){
			clif_guild_allianceack(sd,4);
			clif_guild_allianceack(tsd,3);
			return 0;
		}
		if(tg==NULL || guild_get_alliance_count(tg,0) >= battle_config.max_guild_alliance){
			clif_guild_allianceack(sd,3);
			clif_guild_allianceack(tsd,4);
			return 0;
		}

		for(i=0;i<MAX_GUILDALLIANCE;i++){
			if(g->alliance[i].guild_id==tsd->status.guild_id &&
				g->alliance[i].opposition==1)
				intif_guild_alliance( sd->status.guild_id,tsd->status.guild_id,
					sd->status.account_id,tsd->status.account_id,9 );
		}
		for(i=0;i<MAX_GUILDALLIANCE;i++){
			if(tg->alliance[i].guild_id==sd->status.guild_id &&
				tg->alliance[i].opposition==1)
				intif_guild_alliance( tsd->status.guild_id,sd->status.guild_id,
					tsd->status.account_id,sd->status.account_id,9 );
		}

	// inform other servers
		intif_guild_alliance( sd->status.guild_id,tsd->status.guild_id,
			sd->status.account_id,tsd->status.account_id,0 );
		return 0;
	} else { // deny
		sd->guild_alliance=0;
		sd->guild_alliance_account=0;
		if(tsd!=NULL)
			clif_guild_allianceack(tsd,3);
	}
	return 0;
}

/*====================================================
 * Player sd asking to break alliance with guild guild_id
 *---------------------------------------------------*/
int guild_delalliance(struct map_session_data *sd,int guild_id,int flag) {
	nullpo_ret(sd);

	if(agit_flag || agit2_flag)	{	// Disable alliance breaking during woe [Valaris]
		clif_displaymessage(sd->fd,msg_txt(sd,677)); //"Alliances cannot be broken during Guild Wars!"
		return 0;
	}	// end addition [Valaris]

	if( flag && !guild_can_breakwar(sd->status.guild_id, guild_id) ) {
		clif_displaymessage(sd->fd,"Opposition cannot be broken at War. You need to wait 10 minutes of No Hostile activity.");
		return 0;
	}

	intif_guild_alliance( sd->status.guild_id,guild_id,sd->status.account_id,0,flag|8 );
	return 0;
}

/*====================================================
 * Player sd, asking player tsd a formal enemy relation between their 2 guilds
 *---------------------------------------------------*/
int guild_opposition(struct map_session_data *sd,struct map_session_data *tsd) {
	struct guild *g;
	int i;

	nullpo_ret(sd);

	g=sd->guild;
	if(g==NULL || tsd==NULL)
		return 0;

	// Prevent creation opposition with same guilds [LuzZza]
	if(sd->status.guild_id == tsd->status.guild_id)
		return 0;

	if( guild_get_alliance_count(g,1) >= battle_config.max_guild_opposition )	{
		clif_guild_oppositionack(sd,1);
		return 0;
	}

	for (i = 0; i < MAX_GUILDALLIANCE; i++) { // checking relations
		if(g->alliance[i].guild_id==tsd->status.guild_id){
			if (g->alliance[i].opposition == 1) { // check if not already hostile
				clif_guild_oppositionack(sd,2);
				return 0;
			}
			if(agit_flag || agit2_flag) // Prevent the changing of alliances to oppositions during WoE.
				return 0;
			//Change alliance to opposition.
			intif_guild_alliance( sd->status.guild_id,tsd->status.guild_id,
				sd->status.account_id,tsd->status.account_id,8 );
		}
	}

	// inform other serv
	intif_guild_alliance( sd->status.guild_id,tsd->status.guild_id,
			sd->status.account_id,tsd->status.account_id,1 );
	return 0;
}

/*====================================================
 * Notification of a relationship between 2 guilds
 *---------------------------------------------------*/
int guild_allianceack(int guild_id1,int guild_id2,uint32 account_id1,uint32 account_id2,int flag,const char *name1,const char *name2)
{
	struct guild *g[2];
	int guild_id[2];
	const char *guild_name[2];
	struct map_session_data *sd[2];
	int j,i;

	guild_id[0] = guild_id1;
	guild_id[1] = guild_id2;
	guild_name[0] = name1;
	guild_name[1] = name2;
	sd[0] = map_id2sd(account_id1);
	sd[1] = map_id2sd(account_id2);

	g[0]=guild_search(guild_id1);
	g[1]=guild_search(guild_id2);

	if(sd[0]!=NULL && (flag&0x0f)==0){
		sd[0]->guild_alliance=0;
		sd[0]->guild_alliance_account=0;
	}

	if (flag & 0x70) { // failure
		for(i=0;i<2-(flag&1);i++)
			if( sd[i]!=NULL )
				clif_guild_allianceack(sd[i],((flag>>4)==i+1)?3:4);
		return 0;
	}

	if (!(flag & 0x08)) { // new relationship
		j = MAX_GUILDALLIANCE;
		for(i=0;i<2-(flag&1);i++) {
			if(g[i]!=NULL) {
				ARR_FIND( 0, MAX_GUILDALLIANCE, j, g[i]->alliance[j].guild_id == 0 );
				if( j < MAX_GUILDALLIANCE ) {
					g[i]->alliance[j].guild_id=guild_id[1-i];
					memcpy(g[i]->alliance[j].name,guild_name[1-i],NAME_LENGTH);
					g[i]->alliance[j].opposition=flag&1;
				}
			}
		}

		// Guild Wars
		if( battle_config.guild_wars && (flag&1) && g[0] && g[1] && j < MAX_GUILDALLIANCE )
		{ // Opossition Created
			char output[256];

			ARR_FIND(0, MAX_GUILDALLIANCE, i, g[1]->alliance[i].guild_id == guild_id[0] && g[1]->alliance[i].opposition != 0);
			if( i < MAX_GUILDALLIANCE )
			{
				sprintf(output, "Guild [%s] and Guild [%s] are now at War!!", g[0]->name, g[1]->name);

				g[0]->alliance[j].war = true;
				g[1]->alliance[i].war = true;
				guild_war_init(g[0], g[1], true);
				guild_war_init(g[1], g[0], true);

				clif_broadcast(NULL, output, strlen(output) + 1, 0, ALL_CLIENT);
			}
		}
	} else { // remove relationship
		for(i=0;i<2-(flag&1);i++) {
			if(g[i]!=NULL) {
				ARR_FIND( 0, MAX_GUILDALLIANCE, j, g[i]->alliance[j].guild_id == guild_id[1-i] && g[i]->alliance[j].opposition == (flag&1) );
				if( j < MAX_GUILDALLIANCE )
				{
					if( battle_config.guild_wars && g[i]->alliance[j].war && (flag&1) )
					{
						guild_war_end(g[0], g[1], true);
						guild_war_end(g[1], g[0], false);
					}

					g[i]->alliance[j].guild_id = 0;
				}
			}
		if (sd[i] != NULL) // notify players
				clif_guild_delalliance(sd[i],guild_id[1-i],(flag&1));
		}
	}

	if ((flag & 0x0f) == 0) { // alliance notification
		if( sd[1]!=NULL )
			clif_guild_allianceack(sd[1],2);
	} else if ((flag & 0x0f) == 1) { // enemy notification
		if( sd[0]!=NULL )
			clif_guild_oppositionack(sd[0],0);
	}


	for (i = 0; i < 2 - (flag & 1); i++) { // Retransmission of the relationship list to all members
		if(g[i]!=NULL)
			for(j=0;j<g[i]->max_member;j++) {
				struct map_session_data *sd_mem = g[i]->member[j].sd;
				if( sd_mem!=NULL){
					clif_guild_allianceinfo(sd_mem);
					channel_gjoin(sd_mem,2); //join ally join
				}
			}
	}
	return 0;
}

/**
 * Notification for the guild disbanded
 * @see DBApply
 */
int guild_broken_sub(DBKey key, DBData *data, va_list ap) {
	struct guild *g = (struct guild *)db_data2ptr(data);
	int guild_id=va_arg(ap,int);
	int i,j;
	struct map_session_data *sd=NULL;

	nullpo_ret(g);

	for(i=0;i<MAX_GUILDALLIANCE;i++){	// Destroy all relationships
		if(g->alliance[i].guild_id==guild_id){
			for(j=0;j<g->max_member;j++)
				if( (sd=g->member[j].sd)!=NULL )
					clif_guild_delalliance(sd,guild_id,g->alliance[i].opposition);
			intif_guild_alliance(g->guild_id, guild_id,0,0,g->alliance[i].opposition|8);
			g->alliance[i].guild_id=0;
		}
	}
	return 0;
}

/**
 * Invoked on Castles when a guild is broken. [Skotlex]
 * @see DBApply
 */
int castle_guild_broken_sub(DBKey key, DBData *data, va_list ap)
{
	struct guild_castle *gc = (struct guild_castle *)db_data2ptr(data);
	int guild_id = va_arg(ap, int);

	nullpo_ret(gc);

	if (gc->guild_id == guild_id) {
		char name[EVENT_NAME_LENGTH];
		// We call castle_event::OnGuildBreak of all castles of the guild
		// You can set all castle_events in the 'db/castle_db.txt'
		safestrncpy(name, gc->castle_event, sizeof(name));
		npc_event_do(strcat(name, "::OnGuildBreak"));

		//Save the new 'owner', this should invoke guardian clean up and other such things.
		guild_castledatasave(gc->castle_id, 1, 0);
	}
	return 0;
}

//Invoked on /breakguild "Guild name"
int guild_broken(int guild_id,int flag) {
	struct guild *g = guild_search(guild_id);
	struct map_session_data *sd = NULL;
	int i;

	if (flag != 0 || g == NULL)
		return 0;

	// Guild Skills Timers
	for( i = 0; i < MAX_GUILDSKILL; ++i )
	{
		if( g->skill_block_timer[i] == INVALID_TIMER )
			continue;
		delete_timer(g->skill_block_timer[i], guild_block_skill_end);
	}

	// Guild Master Cleanup
	if( (sd = map_nick2sd(g->master)) != NULL )
	{
		struct skill_unit_group* group;
		const enum sc_type scs[] = { SC_LEADERSHIP, SC_GLORYWOUNDS, SC_SOULCOLD, SC_HAWKEYES };

		for( i = 0; i < ARRAYLENGTH(scs); i++ )
		{
			if( !sd->sc.data[scs[i]] )
				continue;
			if( (group = skill_id2group(sd->sc.data[scs[i]]->val4)) != NULL )
				skill_delunitgroup(group);
		}

		// Guild Aura Changes here ...
		sd->state.gmaster_flag = 0;
	}

	for (i = 0; i < g->max_member; i++){	// Destroy all relationships
		struct map_session_data *sd = g->member[i].sd;
		if(sd != NULL){
			if(sd->state.storage_flag == 2)
				gstorage_storage_quit(sd,1);
			sd->status.guild_id=0;
			sd->guild = NULL;
			sd->state.gmaster_flag = 0;

			clif_guild_broken(g->member[i].sd,0);
			clif_charnameupdate(sd); // [LuzZza]
			status_change_end(&sd->bl,SC_LEADERSHIP,INVALID_TIMER);
			status_change_end(&sd->bl,SC_GLORYWOUNDS,INVALID_TIMER);
			status_change_end(&sd->bl,SC_SOULCOLD,INVALID_TIMER);
			status_change_end(&sd->bl,SC_HAWKEYES,INVALID_TIMER);
			status_calc_pc(sd,0); // Regional System
		}
	}

	guild_db->foreach(guild_db,guild_broken_sub,guild_id);
	castle_db->foreach(castle_db,castle_guild_broken_sub,guild_id);
	gstorage_delete(guild_id);
	if( channel_config.ally_enable ) {
		channel_delete(g->channel);
	}
	idb_remove(guild_db,guild_id);
	return 0;
}

/** Changes the Guild Master to the specified player. [Skotlex]
* @param guild_id
* @param sd New guild master
*/
int guild_gm_change(int guild_id, struct map_session_data *sd) {
	struct guild *g;
	nullpo_ret(sd);

	if (sd->status.guild_id != guild_id)
		return 0;

	g = guild_search(guild_id);

	nullpo_ret(g);

	if (strcmp(g->master, sd->status.name) == 0) //Nothing to change.
		return 0;

	//Notify servers that master has changed.
	intif_guild_change_gm(guild_id, sd->status.name, strlen(sd->status.name)+1);
	return 1;
}

/** Notification from Char server that a guild's master has changed. [Skotlex]
* @param guild_id
* @param account_id
* @param char_id
*/
int guild_gm_changed(int guild_id, uint32 account_id, uint32 char_id) {
	struct guild *g;
	struct guild_member gm;
	char output[128];
	int pos, i;

	g=guild_search(guild_id);

	if (!g)
		return 0;

	for(pos=0; pos<g->max_member && !(
		g->member[pos].account_id==account_id &&
		g->member[pos].char_id==char_id);
		pos++);

	if (pos == 0 || pos == g->max_member) return 0;

	memcpy(&gm, &g->member[pos], sizeof (struct guild_member));
	memcpy(&g->member[pos], &g->member[0], sizeof(struct guild_member));
	memcpy(&g->member[0], &gm, sizeof(struct guild_member));

	g->member[pos].position = g->member[0].position;
	g->member[0].position = 0; //Position 0: guild Master.
	strcpy(g->master, g->member[0].name);

	sprintf(output, "The Guild Master of [%s] has been changed to [%s]", g->name, g->master);
	clif_broadcast(NULL, output, strlen(output) + 1, 0, ALL_CLIENT);

	if (g->member[pos].sd && g->member[pos].sd->fd) {
		if( battle_config.at_changegm_cost && g->member[pos].sd->status.zeny >= battle_config.at_changegm_cost )
			pc_payzeny(g->member[pos].sd, battle_config.at_changegm_cost,LOG_TYPE_NONE,NULL);

		clif_displaymessage(g->member[pos].sd->fd, msg_txt(g->member[pos].sd,678)); //"You no longer are the Guild Master."
		g->member[pos].sd->state.gmaster_flag = 0;
	}

	if (g->member[0].sd && g->member[0].sd->fd) {
		clif_displaymessage(g->member[0].sd->fd, msg_txt(g->member[pos].sd,679)); //"You have become the Guild Master!"
		g->member[0].sd->state.gmaster_flag = 1;
	}

	// announce the change to all guild members
	for( i = 0; i < g->max_member; i++ ) {
		if( g->member[i].sd && g->member[i].sd->fd && !(battle_config.bg_eAmod_mode && g->member[i].sd->bg_id) ) {
			clif_guild_basicinfo(g->member[i].sd);
			clif_guild_memberlist(g->member[i].sd);
		}
	}

	return 1;
}

/** Disband a guild
* @param sd Player who breaks the guild
* @param name Guild name
*/
int guild_break(struct map_session_data *sd,char *name) {
	struct guild *g;
	struct unit_data *ud;
	int i;
#ifdef BOUND_ITEMS
	int j;
	int idxlist[MAX_INVENTORY];
#endif

	nullpo_ret(sd);

	if ((g=sd->guild)==NULL)
		return 0;
	if (strcmp(g->name,name) != 0)
		return 0;
	if (!sd->state.gmaster_flag)
		return 0;
	for (i = 0; i < g->max_member; i++) {
		if(	g->member[i].account_id>0 && (
			g->member[i].account_id!=sd->status.account_id ||
			g->member[i].char_id!=sd->status.char_id ))
			break;
	}
	if (i < g->max_member) {
		clif_guild_broken(sd,2);
		return 0;
	}

	if (g->instance_id)
		instance_destroy(g->instance_id);

	/* Regardless of char server allowing it, we clear the guild master's auras */
	if ((ud = unit_bl2ud(&sd->bl))) {
		int count = 0;
		struct skill_unit_group *group[4];

		for(i = 0; i < MAX_SKILLUNITGROUP && ud->skillunit[i]; i++) {
			switch(ud->skillunit[i]->skill_id) {
				case GD_LEADERSHIP:
				case GD_GLORYWOUNDS:
				case GD_SOULCOLD:
				case GD_HAWKEYES:
					if(count == 4)
						ShowWarning("guild_break: '%s' got more than 4 guild aura instances! (%d)\n",sd->status.name,ud->skillunit[i]->skill_id);
					else
						group[count++] = ud->skillunit[i];
					break;
			}
		}
		for (i = 0; i < count; i++)
			skill_delunitgroup(group[i]);
	}

#ifdef BOUND_ITEMS
	//Guild bound item check - Removes the bound flag
	j = pc_bound_chk(sd,BOUND_GUILD,idxlist);
	for(i = 0; i < j; i++)
		pc_delitem(sd,idxlist[i],sd->status.inventory[idxlist[i]].amount,0,1,LOG_TYPE_BOUND_REMOVAL);
#endif

	intif_guild_break(g->guild_id);
	return 1;
}

/**
 * Creates a list of guild castle IDs to be requested
 * from char-server.
 */
void guild_castle_map_init(void) {
	int num = db_size(castle_db);

	if (num > 0) {
		struct guild_castle* gc = NULL;
		int *castle_ids, *cursor;
		DBIterator* iter = NULL;

		CREATE(castle_ids, int, num);
		cursor = castle_ids;
		iter = db_iterator(castle_db);
		for (gc = (struct guild_castle*)dbi_first(iter); dbi_exists(iter); gc = (struct guild_castle*)dbi_next(iter)) {
			*(cursor++) = gc->castle_id;
		}
		dbi_destroy(iter);
		if (intif_guild_castle_dataload(num, castle_ids))
			ShowStatus("Requested '"CL_WHITE"%d"CL_RESET"' guild castles from char-server...\n", num);
		aFree(castle_ids);
	}
}

/**
 * Setter function for members of guild_castle struct.
 * Handles all side-effects, like updating guardians.
 * Sends updated info to char-server for saving.
 * @param castle_id Castle ID
 * @param index Type of data to change
 * @param value New value
 */
int guild_castledatasave(int castle_id, int index, int value) {
	struct guild_castle *gc = guild_castle_search(castle_id);

	if (gc == NULL) {
		ShowWarning("guild_castledatasave: guild castle '%d' not found\n", castle_id);
		return 0;
	}

	switch (index) {
	case 1: // The castle's owner has changed? Update or remove Guardians too. [Skotlex]
	{
		int i;
		struct guild *g;
		int m = map_mapindex2mapid(gc->mapindex);
		if( map_allowed_woe(m) && gc->guild_id && (g = guild_search(gc->guild_id)) != NULL )
		{ // Current WoE
			int i = gc->castle_id,
				addtime = DIFF_TICK(last_tick, gc->capture_tick),
				score = (addtime / 300) * (1 + (gc->economy / 25));

			g->castle[i].posesion_time += addtime;
			g->castle[i].defensive_score += score;
			g->castle[i].changed = true;
		}

		gc->capture_tick = last_tick;
		gc->guild_id = value;
		for (i = 0; i < MAX_GUARDIANS; i++){
			struct mob_data *gd;
			if (gc->guardian[i].visible && (gd = map_id2md(gc->guardian[i].id)) != NULL)
				mob_guardian_guildchange(gd);
		}
		break;
	}
	case 2:
	{
		struct guild *g = gc->guild_id ? guild_search(gc->guild_id) : NULL;
		if( g && gc->economy < value )
		{
			int eco = value - gc->economy;
			add2limit(g->castle[gc->castle_id].invest_eco, eco, USHRT_MAX);
			if( g->castle[gc->castle_id].top_eco < value )
				g->castle[gc->castle_id].top_eco = value;
			g->castle[gc->castle_id].changed = true;
			if( !agit_flag )
			{
				intif_guild_save_score(g->guild_id, gc->castle_id, &g->castle[gc->castle_id]);
				g->castle[gc->castle_id].changed = false;
			}
		}
		gc->economy = value;
		break;
	}
	case 3: // defense invest change -> recalculate guardian hp
	{
		int i;
		struct guild *g = gc->guild_id ? guild_search(gc->guild_id) : NULL;
		if( g && gc->defense < value )
		{
			int def = value - gc->defense;
			add2limit(g->castle[gc->castle_id].invest_def, def, USHRT_MAX);
			if( g->castle[gc->castle_id].top_def < value )
				g->castle[gc->castle_id].top_def = value;
			g->castle[gc->castle_id].changed = true;
			if( !agit_flag )
			{
				intif_guild_save_score(g->guild_id, gc->castle_id, &g->castle[gc->castle_id]);
				g->castle[gc->castle_id].changed = false;
			}
		}
		gc->defense = value;
		for (i = 0; i < MAX_GUARDIANS; i++){
			struct mob_data *gd;
			if (gc->guardian[i].visible && (gd = map_id2md(gc->guardian[i].id)) != NULL)
				status_calc_mob(gd, SCO_NONE);
		}
		break;
	}
	case 4:
		gc->triggerE = value; break;
	case 5:
		gc->triggerD = value; break;
	case 6:
		gc->nextTime = value; break;
	case 7:
		gc->payTime = value; break;
	case 8:
		gc->createTime = value; break;
	case 9:
		gc->visibleC = value; break;
	default:
		if (index > 9 && index <= 9+MAX_GUARDIANS) {
			gc->guardian[index-10].visible = value;
			break;
		}
		ShowWarning("guild_castledatasave: index = '%d' is out of allowed range\n", index);
		return 0;
	}

	if (!intif_guild_castle_datasave(castle_id, index, value)) {
		guild_castle_reconnect(castle_id, index, value);
	}
	return 0;
}

void guild_castle_reconnect_sub(void *key, void *data, va_list ap) {
	int castle_id = GetWord((int)__64BPRTSIZE(key), 0);
	int index = GetWord((int)__64BPRTSIZE(key), 1);
	intif_guild_castle_datasave(castle_id, index, *(int *)data);
	aFree(data);
}

/**
 * Saves pending guild castle data changes when char-server is
 * disconnected.
 * On reconnect pushes all changes to char-server for saving.
 * @param castle_id
 * @param index
 * @param value
 */
void guild_castle_reconnect(int castle_id, int index, int value) {
	static struct linkdb_node *gc_save_pending = NULL;

	if (castle_id < 0) { // char-server reconnected
		linkdb_foreach(&gc_save_pending, guild_castle_reconnect_sub);
		linkdb_final(&gc_save_pending);
	} else {
		int *data;
		CREATE(data, int, 1);
		*data = value;
		linkdb_replace(&gc_save_pending, (void*)__64BPRTSIZE((MakeDWord(castle_id, index))), data);
	}
}

/** Load castle data then invoke OnAgitInit* on last
* @param len
* @param gc Guild Castle data
*/
int guild_castledataloadack(int len, struct guild_castle *gc) {
	int i;
	int n = (len-4) / sizeof(struct guild_castle);
	int ev;

	nullpo_ret(gc);

	//Last owned castle in the list invokes ::OnAgitInit
	for( i = n-1; i >= 0 && !(gc[i].guild_id); --i );
	ev = i; // offset of castle or -1

	if( ev < 0 ) { //No castles owned, invoke OnAgitInit as it is.
		npc_event_doall("OnAgitInit");
		npc_event_doall("OnAgitInit2");
	} else // load received castles into memory, one by one
	for( i = 0; i < n; i++, gc++ ) {
		struct guild_castle *c = guild_castle_search(gc->castle_id);
		if (!c) {
			ShowError("guild_castledataloadack: castle id=%d not found.\n", gc->castle_id);
			continue;
		}

		// update map-server castle data with new info
		memcpy(&c->guild_id, &gc->guild_id, sizeof(struct guild_castle) - offsetof(struct guild_castle, guild_id));

		if( c->guild_id ) {
			if( i != ev )
				guild_request_info(c->guild_id);
			else { // last owned one
				guild_npc_request_info(c->guild_id, "::OnAgitInit");
				guild_npc_request_info(c->guild_id, "::OnAgitInit2");
			}
		}
	}
	ShowStatus("Received '"CL_WHITE"%d"CL_RESET"' guild castles from char-server.\n", n);
	return 0;
}

/*------------------------------------------
 * Guild Ranking System
 *------------------------------------------*/
int guild_ranking_save(int flag)
{
	struct guild_castle *gc;
	struct guild *g;
	DBIterator* iter;
	struct map_session_data *sd;
	int i, j, m, index, cc;

	iter = castle_db->iterator(castle_db);
	for( gc = (struct guild_castle*)dbi_first(iter); dbi_exists(iter); gc = (struct guild_castle*)dbi_next(iter) )
	{
		if( gc->guild_id == 0 )
			continue;
		
		if( woe_set && (m = map_mapindex2mapid(gc->mapindex)) >= 0 && map[m].flag.woe_set != woe_set )
			continue; // Not considered on this ranking

		index = gc->castle_id;

		if( index >= RANK_CASTLES || (flag == 1 && index >= 24) || (flag == 2 && index < 24) )
			continue;

		if( (g = guild_search(gc->guild_id)) != NULL )
		{
			int addtime = DIFF_TICK(last_tick, gc->capture_tick),
				score = (addtime / 300) * (1 + (gc->economy / 25));

			g->castle[index].capture++;
			g->castle[index].posesion_time += addtime;
			g->castle[index].defensive_score += score;
			g->castle[index].changed = true;

			// Capture counter for members
			for( j = 0; j < MAX_GUILD; j++ )
			{
				if( (sd = g->member[j].sd) == NULL )
					continue;

				cc = pc_readaccountreg(sd,add_str("#GC_CAPTURES"));
				pc_setaccountreg(sd,add_str("#GC_CAPTURES"),++cc);
			}
		}
	}
	iter->destroy(iter);

	iter = guild_db->iterator(guild_db);
	for( g = (struct guild*)dbi_first(iter); dbi_exists(iter); g = (struct guild*)dbi_next(iter) )
	{
		for( i = 0; i < RANK_CASTLES; i++ )
		{
			if( !g->castle[i].changed )
				continue;

			intif_guild_save_score(g->guild_id, i, &g->castle[i]);
			g->castle[i].changed = false;
		}
	}
	iter->destroy(iter);
	return 0;
}

/*====================================================
 * Start normal woe and triggers all npc OnAgitStart
 *---------------------------------------------------*/
void guild_agit_start(void) {	// Run All NPC_Event[OnAgitStart]
	int c = npc_event_doall("OnAgitStart");
	struct guild_castle* gc;
	DBIterator *iter = db_iterator(castle_db);
	ShowStatus("NPC_Event:[OnAgitStart] Run (%d) Events by @AgitStart.\n",c);

	for( gc = (struct guild_castle*)dbi_first(iter); dbi_exists(iter); gc = (struct guild_castle*)dbi_next(iter) )
	{
		if( gc->castle_id >= 24 )
			continue; // WoE SE Castle
		if( !gc->guild_id )
			continue; // No owner

		gc->capture_tick = last_tick;
	}
	dbi_destroy(iter);
}

/*====================================================
 * End normal woe and triggers all npc OnAgitEnd
 *---------------------------------------------------*/
void guild_agit_end(void) {	// Run All NPC_Event[OnAgitEnd]
	int c = npc_event_doall("OnAgitEnd");
	ShowStatus("NPC_Event:[OnAgitEnd] Run (%d) Events by @AgitEnd.\n",c);
	// Stop auto saving
	guild_ranking_save(1);
}

/*====================================================
 * Start woe2 and triggers all npc OnAgitStart2
 *---------------------------------------------------*/
void guild_agit2_start(void) {	// Run All NPC_Event[OnAgitStart2]
	int c = npc_event_doall("OnAgitStart2");
	struct guild_castle* gc;
	DBIterator *iter = db_iterator(castle_db);
	ShowStatus("NPC_Event:[OnAgitStart2] Run (%d) Events by @AgitStart2.\n",c);

	for( gc = (struct guild_castle*)dbi_first(iter); dbi_exists(iter); gc = (struct guild_castle*)dbi_next(iter) )
	{
		if( gc->castle_id < 24 )
			continue; // Non WoE SE Castle
		if( !gc->guild_id )
			continue; // No owner

		gc->capture_tick = last_tick;
	}
	dbi_destroy(iter);
}

/*====================================================
 * End woe2 and triggers all npc OnAgitEnd2
 *---------------------------------------------------*/
void guild_agit2_end(void) {	// Run All NPC_Event[OnAgitEnd2]
	int c = npc_event_doall("OnAgitEnd2");
	ShowStatus("NPC_Event:[OnAgitEnd2] Run (%d) Events by @AgitEnd2.\n",c);
	// Stop auto saving
	guild_ranking_save(2);
}

// How many castles does this guild have?
int guild_checkcastles(struct guild *g) {
	int nb_cas = 0;
	struct guild_castle* gc = NULL;
	DBIterator *iter = db_iterator(castle_db);

	for (gc = (struct guild_castle*)dbi_first(iter); dbi_exists(iter); gc = (struct guild_castle*)dbi_next(iter)) {
		if (gc->guild_id == g->guild_id) {
			nb_cas++;
		}
	}
	dbi_destroy(iter);
	return nb_cas;
}

// Are these two guilds allied?
bool guild_isallied(int guild_id, int guild_id2) {
	int i;
	struct guild* g = guild_search(guild_id);
	nullpo_ret(g);

	ARR_FIND( 0, MAX_GUILDALLIANCE, i, g->alliance[i].guild_id == guild_id2 );
	return( i < MAX_GUILDALLIANCE && g->alliance[i].opposition == 0 );
}

void guild_flag_add(struct npc_data *nd) {
	int i;

	/* check */
	for( i = 0; i < guild_flags_count; i++ ) {
		if( guild_flags[i] && guild_flags[i]->bl.id == nd->bl.id ) {
			return;/* exists, most likely updated the id. */
		}
	}

	i = guild_flags_count;/* save the current slot */
	/* add */
	RECREATE(guild_flags,struct npc_data*,++guild_flags_count);
	/* save */
	guild_flags[i] = nd;
}

void guild_flag_remove(struct npc_data *nd) {
	int i, cursor;
	if( guild_flags_count == 0 )
		return;
	/* find it */
	for( i = 0; i < guild_flags_count; i++ ) {
		if( guild_flags[i] && guild_flags[i]->bl.id == nd->bl.id ) {/* found */
			guild_flags[i] = NULL;
			break;
		}
	}

	/* compact list */
	for( i = 0, cursor = 0; i < guild_flags_count; i++ ) {
		if( guild_flags[i] == NULL )
			continue;

		if( cursor != i ) {
			memmove(&guild_flags[cursor], &guild_flags[i], sizeof(struct npc_data*));
		}

		cursor++;
	}
}

/**
 * @see DBApply
 */
static int eventlist_db_final(DBKey key, DBData *data, va_list ap) {
	struct eventlist *next = NULL;
	struct eventlist *current = (struct eventlist *)db_data2ptr(data);
	while (current != NULL) {
		next = current->next;
		aFree(current);
		current = next;
	}
	return 0;
}

/**
 * @see DBApply
 */
static int guild_expcache_db_final(DBKey key, DBData *data, va_list ap) {
	ers_free(expcache_ers, db_data2ptr(data));
	return 0;
}

/**
 * @see DBApply
 */
static int guild_castle_db_final(DBKey key, DBData *data, va_list ap) {
	struct guild_castle* gc = (struct guild_castle *)db_data2ptr(data);
	if( gc->temp_guardians )
		aFree(gc->temp_guardians);
	aFree(gc);
	return 0;
}

/* called when scripts are reloaded/unloaded */
void guild_flags_clear(void) {
	int i;
	for( i = 0; i < guild_flags_count; i++ ) {
		if( guild_flags[i] )
			guild_flags[i] = NULL;
	}

	guild_flags_count = 0;
}

void do_init_guild(void) {
	const char* dbsubpath[] = {
		"",
		"/"DBIMPORT,
	};
	int i;
	
	guild_db           = idb_alloc(DB_OPT_RELEASE_DATA);
	castle_db          = idb_alloc(DB_OPT_BASE);
	guild_expcache_db  = idb_alloc(DB_OPT_BASE);
	guild_infoevent_db = idb_alloc(DB_OPT_BASE);
	expcache_ers = ers_new(sizeof(struct guild_expcache),"guild.c::expcache_ers",ERS_OPT_NONE);

	guild_flags_count = 0;

	memset(guild_skill_tree,0,sizeof(guild_skill_tree));
	
	for(i=0; i<ARRAYLENGTH(dbsubpath); i++){
		int n1 = strlen(db_path)+strlen(dbsubpath[i])+1;
		char* dbsubpath1 = (char*)aMalloc(n1+1);
		safesnprintf(dbsubpath1,n1+1,"%s%s",db_path,dbsubpath[i]);
		
		sv_readdb(dbsubpath1, "castle_db.txt", ',', 4, 4, -1, &guild_read_castledb, i);
		sv_readdb(dbsubpath1, "guild_skill_tree.txt", ',', 2+MAX_GUILD_SKILL_REQUIRE*2, 2+MAX_GUILD_SKILL_REQUIRE*2, -1, &guild_read_guildskill_tree_db, i); //guild skill tree [Komurka]
		
		aFree(dbsubpath1);
	}
	
	add_timer_func_list(guild_payexp_timer,"guild_payexp_timer");
	add_timer_func_list(guild_send_xy_timer, "guild_send_xy_timer");
	add_timer_interval(gettick()+GUILD_PAYEXP_INTERVAL,guild_payexp_timer,0,0,GUILD_PAYEXP_INTERVAL);
	add_timer_interval(gettick()+GUILD_SEND_XY_INTERVAL,guild_send_xy_timer,0,0,GUILD_SEND_XY_INTERVAL);
}

void do_final_guild(void) {
	DBIterator *iter = db_iterator(guild_db);
	struct guild *g;

	for( g = (struct guild *)dbi_first(iter); dbi_exists(iter); g = (struct guild *)dbi_next(iter) ) {
		channel_delete(g->channel);
	}
	dbi_destroy(iter);

	db_destroy(guild_db);
	castle_db->destroy(castle_db,guild_castle_db_final);
	guild_expcache_db->destroy(guild_expcache_db,guild_expcache_db_final);
	guild_infoevent_db->destroy(guild_infoevent_db,eventlist_db_final);
	ers_destroy(expcache_ers);

	aFree(guild_flags);/* never empty; created on boot */
}
