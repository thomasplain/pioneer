#include "LuaSerializer.h"
#include "LuaObject.h"
#include "LuaBody.h"
#include "LuaShip.h"
#include "LuaSpaceStation.h"
#include "LuaPlanet.h"
#include "LuaStar.h"
#include "LuaPlayer.h"
#include "LuaSBodyPath.h"
#include "StarSystem.h"
#include "Body.h"
#include "Ship.h"
#include "SpaceStation.h"
#include "Planet.h"
#include "Star.h"
#include "Player.h"

// every module can save one object. that will usually be a table.  we call
// each serializer in turn and capture its return value we build a table like
// so:
// {
//   'Assassination' = { ... },
//   'DeliverPackage' = { ... },
//   ...
// }
// entire table then gets pickled and handed to the writer
//
// on load, we unpickle the table then call the registered unserialize
// function for each module with its table
//
// we keep a copy of this table around. next time we save we overwrite the
// each individual module's data. that way if a player loads a game with dat
// for a module that is not currently loaded, we don't lose its data in the
// next save


// pickler can handle simple types (boolean, number, string) and will drill
// down into tables. it can do userdata for a specific set of types - Body and
// its kids and SBodyPath. anything else will cause a lua error
//
// pickle format is newline-seperated. each line begins with a type value,
// followed by data for that type as follows
//   fNNN.nnn - number (float)
//   bN       - boolean. N is 0 or 1 for true/false
//   sNNN     - string. number is length, followed by newline, then string of bytes
//   t        - table. table contents are more pickled stuff (ie recursive)
//   n        - end of table
//   uXXXX    - userdata. XXXX is type, followed by newline, followed by data
//     Body      - data is a single stringified number for Serializer::LookupBody
//     SBodyPath - data is four stringified numbers, newline separated

void LuaSerializer::pickle(lua_State *l, int idx, std::string &out, const char *key = NULL)
{
	static char buf[256];

	LUA_DEBUG_START(l);

	switch (lua_type(l, idx)) {
		case LUA_TNIL:
			break;

		case LUA_TNUMBER: {
			snprintf(buf, sizeof(buf), "f%f\n", lua_tonumber(l, idx));
			out += buf;
			break;
		}

		case LUA_TBOOLEAN: {
			snprintf(buf, sizeof(buf), "b%d", lua_toboolean(l, idx) ? 1 : 0);
			out += buf;
			break;
		}

		case LUA_TSTRING: {
			lua_pushvalue(l, idx);
			const char *str = lua_tostring(l, -1);
			snprintf(buf, sizeof(buf), "s%d\n", strlen(str));
			out += buf;
			out += str;
			lua_pop(l, 1);
			break;
		}

		case LUA_TTABLE: {
			out += "t";
			lua_pushvalue(l, idx);
			lua_pushnil(l);
			while (lua_next(l, -2)) {
				if (key) {
					pickle(l, -2, out, key);
					pickle(l, -1, out, key);
				}
				else {
					lua_pushvalue(l, -2);
					const char *k = lua_tostring(l, -1);
					pickle(l, -3, out, k);
					pickle(l, -2, out, k);
					lua_pop(l, 1);
				}
				lua_pop(l, 1);
			}
			lua_pop(l, 1);
			out += "n";
			break;
		}

		case LUA_TUSERDATA: {
			out += "u";
			lid *idp = (lid*)lua_touserdata(l, idx);
			LuaObjectBase *lo = LuaObjectBase::Lookup(*idp);
			if (!lo)
				Error("Lua serializer '%s' tried to serialize object with id 0x%08x, but it no longer exists", key, *idp);

			if (lo->Isa("SBodyPath")) {
				SBodyPath *sbp = dynamic_cast<SBodyPath*>(lo->m_object);
				snprintf(buf, sizeof(buf), "SBodyPath\n%d\n%d\n%d\n%d\n", sbp->sectorX, sbp->sectorY, sbp->systemNum, sbp->sbodyId);
				out += buf;
				break;
			}

			if (lo->Isa("Body")) {
				Body *b = dynamic_cast<Body*>(lo->m_object);
				snprintf(buf, sizeof(buf), "Body\n%d\n", Serializer::LookupBody(b));
				out += buf;
				break;
			}

			Error("Lua serializer '%s' tried to serialize unsupported userdata value", key);
			break;
		}

		default:
			Error("Lua serializer '%s' tried to serialize %s value", key, lua_typename(l, lua_type(l, idx)));
			break;
	}

	LUA_DEBUG_END(l, 0);
}

const char *LuaSerializer::unpickle(lua_State *l, const char *pos)
{
	LUA_DEBUG_START(l);

	char type = *pos++;

	switch (type) {

		case 'f': {
			char *end;
			double f = strtod(pos, &end);
			if (pos == end) throw SavedGameCorruptException();
			lua_pushnumber(l, f);
			pos = end+1; // skip newline
			break;
		}

		case 'b': {
			if (*pos != '0' && *pos != '1') throw SavedGameCorruptException();
			bool b = (*pos == '0') ? false : true;
			lua_pushboolean(l, b);
			pos++;
			break;
		}

		case 's': {
			char *end;
			int len = strtod(pos, &end);
			if (pos == end) throw SavedGameCorruptException();
			end++; // skip newline
			lua_pushlstring(l, end, len);
			pos = end + len;
			break;
		}
			
		case 't': {
			lua_newtable(l);
			while (*pos != 'n') {
				pos = unpickle(l, pos);
				pos = unpickle(l, pos);
				lua_rawset(l, -3);
			}
			pos++;
			break;
		}

		case 'u': {
			const char *end = strchr(pos, '\n');
			if (!end) throw SavedGameCorruptException();
			int len = end - pos;
			end++; // skip newline

			if (len == 9 && strncmp(pos, "SBodyPath", 9) == 0) {
				pos = end;

				int sectorX = strtol(pos, (char**)&end, 0);
				if (pos == end) throw SavedGameCorruptException();
				pos = end+1; // skip newline

				int sectorY = strtol(pos, (char**)&end, 0);
				if (pos == end) throw SavedGameCorruptException();
				pos = end+1; // skip newline

				int systemNum = strtol(pos, (char**)&end, 0);
				if (pos == end) throw SavedGameCorruptException();
				pos = end+1; // skip newline

				int sbodyId = strtol(pos, (char**)&end, 0);
				if (pos == end) throw SavedGameCorruptException();
				pos = end+1; // skip newline

				SBodyPath *sbp = new SBodyPath(sectorX, sectorY, systemNum);
				sbp->sbodyId = sbodyId;
				LuaSBodyPath::PushToLuaGC(sbp);

				break;
			}

			if (len == 4 && strncmp(pos, "Body", 4) == 0) {
				pos = end;

				int n = strtol(pos, (char**)&end, 0);
				if (pos == end) throw SavedGameCorruptException();
				pos = end+1; // skip newline

				Body *body = Serializer::LookupBody(n);
				if (pos == end) throw SavedGameCorruptException();

				switch (body->GetType()) {
					case Object::BODY:
						LuaBody::PushToLua(body);
						break;
					case Object::SHIP:
						LuaShip::PushToLua(dynamic_cast<Ship*>(body));
						break;
					case Object::SPACESTATION:
						LuaSpaceStation::PushToLua(dynamic_cast<SpaceStation*>(body));
						break;
					case Object::PLANET:
						LuaPlanet::PushToLua(dynamic_cast<Planet*>(body));
						break;
					case Object::STAR:
						LuaStar::PushToLua(dynamic_cast<Star*>(body));
						break;
					case Object::PLAYER:
						LuaPlayer::PushToLua(dynamic_cast<Player*>(body));
						break;
					default:
						throw SavedGameCorruptException();
				}

				break;
			}

			throw SavedGameCorruptException();
		}

		default:
			throw SavedGameCorruptException();
	}

	LUA_DEBUG_END(l, 1);

	return pos;
}

void LuaSerializer::Serialize(Serializer::Writer &wr)
{
	lua_State *l = LuaManager::Instance()->GetLuaState();

	LUA_DEBUG_START(l);

	lua_newtable(l);
	int savetable = lua_gettop(l);

	lua_getfield(l, LUA_REGISTRYINDEX, "PiSerializerCallbacks");
	if (lua_isnil(l, -1)) {
		lua_pop(l, 1);
		lua_newtable(l);
		lua_pushvalue(l, -1);
		lua_setfield(l, LUA_REGISTRYINDEX, "PiSerializerCallbacks");
	}

	lua_pushnil(l);
	while (lua_next(l, -2) != 0) {
		lua_pushinteger(l, 1);
		lua_gettable(l, -2);
		lua_call(l, 0, 1);
		lua_pushvalue(l, -3);
		lua_insert(l, -2);
		lua_settable(l, savetable);
		lua_pop(l, 1);
	}

	lua_pop(l, 1);

	std::string pickled;
	pickle(l, savetable, pickled);

	wr.String(pickled);

	lua_pop(l, 1);

	LUA_DEBUG_END(l, 0);
}

void LuaSerializer::Unserialize(Serializer::Reader &rd)
{
	lua_State *l = LuaManager::Instance()->GetLuaState();

	LUA_DEBUG_START(l);

	std::string pickled = rd.String();
	const char *start = pickled.c_str();
	const char *end = unpickle(l, start);
	if ((end - start) != pickled.length()) throw SavedGameCorruptException();
	if (!lua_istable(l, -1)) throw SavedGameCorruptException();
	int savetable = lua_gettop(l);

	lua_getfield(l, LUA_REGISTRYINDEX, "PiSerializerCallbacks");
	if (lua_isnil(l, -1)) {
		lua_pop(l, 1);
		lua_newtable(l);
		lua_pushvalue(l, -1);
		lua_setfield(l, LUA_REGISTRYINDEX, "PiSerializerCallbacks");
	}

	lua_pushnil(l);
	while (lua_next(l, -2) != 0) {
		lua_pushvalue(l, -2);
		lua_pushinteger(l, 2);
		lua_gettable(l, -3);
		lua_getfield(l, savetable, lua_tostring(l, -2));
		if (lua_isnil(l, -1)) {
			lua_pop(l, 1);
			lua_newtable(l);
		}
		lua_call(l, 1, 0);
		lua_pop(l, 2);
	}

	lua_pop(l, 2);

	LUA_DEBUG_END(l, 0);
}

int LuaSerializer::l_register(lua_State *l)
{
	LUA_DEBUG_START(l);

	std::string key = luaL_checkstring(l, 2);

	if (!lua_isfunction(l, 3))
		luaL_typerror(l, 3, lua_typename(l, LUA_TFUNCTION));
	if (!lua_isfunction(l, 4))
		luaL_typerror(l, 4, lua_typename(l, LUA_TFUNCTION));
	
	lua_getfield(l, LUA_REGISTRYINDEX, "PiSerializerCallbacks");
	if (lua_isnil(l, -1)) {
		lua_pop(l, 1);
		lua_newtable(l);
		lua_pushvalue(l, -1);
		lua_setfield(l, LUA_REGISTRYINDEX, "PiSerializerCallbacks");
	}

	lua_getfield(l, -1, key.c_str());
	if(!(lua_isnil(l, -1)))
		luaL_error(l, "Lua serializer functions for '%s' are already registered\n", key.c_str());
	lua_pop(l, 1);

	lua_newtable(l);

	lua_pushinteger(l, 1);
	lua_pushvalue(l, 3);
	lua_rawset(l, -3);
	lua_pushinteger(l, 2);
	lua_pushvalue(l, 4);
	lua_rawset(l, -3);

	lua_pushstring(l, key.c_str());
	lua_pushvalue(l, -2);
	lua_rawset(l, -4);

	lua_pop(l, 2);

	LUA_DEBUG_END(l, 0);

	return 0;
}

template <> const char *LuaObject<LuaSerializer>::s_type = "Serializer";

template <> void LuaObject<LuaSerializer>::RegisterClass()
{
	static const luaL_reg l_methods[] = {
		{ "Register", LuaSerializer::l_register },
		{ 0, 0 }
	};

	LuaObjectBase::CreateClass(s_type, NULL, l_methods, NULL);
}