/*
 * COSMOS FAST SPATIAL V1
 * Native spatial index for RPG Maker VX Ace / mkxp-z / MRI.
 *
 * Purpose:
 *   Move repeated MOBA proximity searches out of Ruby.
 *
 * Ruby module exposed:
 *   CosmosFastSpatial
 *
 * API:
 *   configure(cell_size_units)
 *   clear
 *   upsert(id, x_units, y_units, team_mask, kind_mask)
 *   remove(id)
 *   query_radius(x_units, y_units, radius_units, team_mask = 0, kind_mask = 0)
 *   nearest(x_units, y_units, radius_units, team_mask = 0, kind_mask = 0, exclude_id = 0)
 *   stats
 *
 * Masks:
 *   TEAM_ALLY    = 1
 *   TEAM_ENEMY   = 2
 *   TEAM_NEUTRAL = 4
 *
 *   KIND_CHAMPION = 1
 *   KIND_MINION   = 2
 *   KIND_PET      = 4
 *   KIND_OTHER    = 8
 */

#include "binding-util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace
{
    static const int TEAM_ALLY    = 1;
    static const int TEAM_ENEMY   = 2;
    static const int TEAM_NEUTRAL = 4;

    static const int KIND_CHAMPION = 1;
    static const int KIND_MINION   = 2;
    static const int KIND_PET      = 4;
    static const int KIND_OTHER    = 8;

    struct Entry
    {
        long long id;
        double x;
        double y;
        int team;
        int kind;
        int cellX;
        int cellY;
    };

    struct Hit
    {
        long long id;
        double d2;
    };

    static double g_cellSize = 500.0;

    static std::unordered_map<long long, Entry> g_entries;
    static std::unordered_map<std::uint64_t, std::vector<long long> > g_cells;

    static inline int cellCoord(double value)
    {
        return static_cast<int>(std::floor(value / g_cellSize));
    }

    static inline std::uint64_t cellKey(int cx, int cy)
    {
        const std::uint64_t ux = static_cast<std::uint32_t>(cx);
        const std::uint64_t uy = static_cast<std::uint32_t>(cy);
        return (ux << 32) | uy;
    }

    static inline bool matchesMask(const Entry &entry, int teamMask, int kindMask)
    {
        if (teamMask != 0 && (entry.team & teamMask) == 0)
            return false;

        if (kindMask != 0 && (entry.kind & kindMask) == 0)
            return false;

        return true;
    }

    static void removeFromCell(long long id, int cx, int cy)
    {
        const std::uint64_t key = cellKey(cx, cy);
        auto it = g_cells.find(key);
        if (it == g_cells.end())
            return;

        std::vector<long long> &ids = it->second;
        auto pos = std::find(ids.begin(), ids.end(), id);

        if (pos != ids.end())
            ids.erase(pos);

        if (ids.empty())
            g_cells.erase(it);
    }

    static void addToCell(long long id, int cx, int cy)
    {
        g_cells[cellKey(cx, cy)].push_back(id);
    }

    static void rebuildCells()
    {
        g_cells.clear();

        for (auto &pair : g_entries)
        {
            Entry &entry = pair.second;
            entry.cellX = cellCoord(entry.x);
            entry.cellY = cellCoord(entry.y);
            addToCell(entry.id, entry.cellX, entry.cellY);
        }
    }

    static std::vector<Hit> collectRadius(
        double x,
        double y,
        double radius,
        int teamMask,
        int kindMask,
        long long excludeId)
    {
        std::vector<Hit> hits;

        if (radius < 0.0)
            return hits;

        const double radiusSq = radius * radius;

        const int minCX = cellCoord(x - radius);
        const int maxCX = cellCoord(x + radius);
        const int minCY = cellCoord(y - radius);
        const int maxCY = cellCoord(y + radius);

        for (int cx = minCX; cx <= maxCX; ++cx)
        {
            for (int cy = minCY; cy <= maxCY; ++cy)
            {
                auto cellIt = g_cells.find(cellKey(cx, cy));
                if (cellIt == g_cells.end())
                    continue;

                const std::vector<long long> &ids = cellIt->second;

                for (long long id : ids)
                {
                    if (excludeId != 0 && id == excludeId)
                        continue;

                    auto entryIt = g_entries.find(id);
                    if (entryIt == g_entries.end())
                        continue;

                    const Entry &entry = entryIt->second;

                    if (!matchesMask(entry, teamMask, kindMask))
                        continue;

                    const double dx = entry.x - x;
                    const double dy = entry.y - y;
                    const double d2 = dx * dx + dy * dy;

                    if (d2 > radiusSq)
                        continue;

                    Hit hit;
                    hit.id = entry.id;
                    hit.d2 = d2;
                    hits.push_back(hit);
                }
            }
        }

        std::sort(
            hits.begin(),
            hits.end(),
            [](const Hit &a, const Hit &b)
            {
                if (a.d2 < b.d2)
                    return true;
                if (a.d2 > b.d2)
                    return false;
                return a.id < b.id;
            });

        return hits;
    }

    RB_METHOD(cosmosFastSpatialConfigure)
    {
        rb_check_argc(argc, 1);

        const double value = NUM2DBL(argv[0]);

        if (!(value >= 32.0) || !std::isfinite(value))
            rb_raise(rb_eArgError, "cell size must be a finite number >= 32");

        if (std::fabs(value - g_cellSize) > 0.0001)
        {
            g_cellSize = value;
            rebuildCells();
        }

        return rb_float_new(g_cellSize);
    }

    RB_METHOD(cosmosFastSpatialClear)
    {
        RB_UNUSED_PARAM;

        g_entries.clear();
        g_cells.clear();

        return Qnil;
    }

    RB_METHOD(cosmosFastSpatialUpsert)
    {
        rb_check_argc(argc, 5);

        const long long id = NUM2LL(argv[0]);
        const double x = NUM2DBL(argv[1]);
        const double y = NUM2DBL(argv[2]);
        const int team = NUM2INT(argv[3]);
        const int kind = NUM2INT(argv[4]);

        if (id == 0)
            rb_raise(rb_eArgError, "id must be non-zero");

        if (!std::isfinite(x) || !std::isfinite(y))
            rb_raise(rb_eArgError, "x/y must be finite");

        const int newCX = cellCoord(x);
        const int newCY = cellCoord(y);

        auto it = g_entries.find(id);

        if (it == g_entries.end())
        {
            Entry entry;
            entry.id = id;
            entry.x = x;
            entry.y = y;
            entry.team = team;
            entry.kind = kind;
            entry.cellX = newCX;
            entry.cellY = newCY;

            g_entries.emplace(id, entry);
            addToCell(id, newCX, newCY);
        }
        else
        {
            Entry &entry = it->second;

            if (entry.cellX != newCX || entry.cellY != newCY)
            {
                removeFromCell(id, entry.cellX, entry.cellY);
                addToCell(id, newCX, newCY);
            }

            entry.x = x;
            entry.y = y;
            entry.team = team;
            entry.kind = kind;
            entry.cellX = newCX;
            entry.cellY = newCY;
        }

        return argv[0];
    }

    RB_METHOD(cosmosFastSpatialRemove)
    {
        rb_check_argc(argc, 1);

        const long long id = NUM2LL(argv[0]);

        auto it = g_entries.find(id);
        if (it == g_entries.end())
            return Qfalse;

        removeFromCell(id, it->second.cellX, it->second.cellY);
        g_entries.erase(it);

        return Qtrue;
    }

    RB_METHOD(cosmosFastSpatialQueryRadius)
    {
        if (argc < 3 || argc > 5)
            rb_raise(rb_eArgError, "wrong number of arguments (%d for 3..5)", argc);

        const double x = NUM2DBL(argv[0]);
        const double y = NUM2DBL(argv[1]);
        const double radius = NUM2DBL(argv[2]);

        const int teamMask =
            (argc >= 4 && !NIL_P(argv[3])) ? NUM2INT(argv[3]) : 0;

        const int kindMask =
            (argc >= 5 && !NIL_P(argv[4])) ? NUM2INT(argv[4]) : 0;

        std::vector<Hit> hits =
            collectRadius(x, y, radius, teamMask, kindMask, 0);

        VALUE result = rb_ary_new_capa(static_cast<long>(hits.size()));

        for (const Hit &hit : hits)
            rb_ary_push(result, LL2NUM(hit.id));

        return result;
    }

    RB_METHOD(cosmosFastSpatialNearest)
    {
        if (argc < 3 || argc > 6)
            rb_raise(rb_eArgError, "wrong number of arguments (%d for 3..6)", argc);

        const double x = NUM2DBL(argv[0]);
        const double y = NUM2DBL(argv[1]);
        const double radius = NUM2DBL(argv[2]);

        const int teamMask =
            (argc >= 4 && !NIL_P(argv[3])) ? NUM2INT(argv[3]) : 0;

        const int kindMask =
            (argc >= 5 && !NIL_P(argv[4])) ? NUM2INT(argv[4]) : 0;

        const long long excludeId =
            (argc >= 6 && !NIL_P(argv[5])) ? NUM2LL(argv[5]) : 0;

        std::vector<Hit> hits =
            collectRadius(x, y, radius, teamMask, kindMask, excludeId);

        if (hits.empty())
            return Qnil;

        return LL2NUM(hits.front().id);
    }

    RB_METHOD(cosmosFastSpatialStats)
    {
        RB_UNUSED_PARAM;

        VALUE hash = rb_hash_new();

        rb_hash_aset(
            hash,
            ID2SYM(rb_intern("entries")),
            ULL2NUM(static_cast<unsigned long long>(g_entries.size())));

        rb_hash_aset(
            hash,
            ID2SYM(rb_intern("cells")),
            ULL2NUM(static_cast<unsigned long long>(g_cells.size())));

        rb_hash_aset(
            hash,
            ID2SYM(rb_intern("cell_size")),
            rb_float_new(g_cellSize));

        return hash;
    }
}

void cosmosFastSpatialBindingInit()
{
    VALUE mod = rb_define_module("CosmosFastSpatial");

    _rb_define_module_function(mod, "configure", cosmosFastSpatialConfigure);
    _rb_define_module_function(mod, "clear", cosmosFastSpatialClear);
    _rb_define_module_function(mod, "upsert", cosmosFastSpatialUpsert);
    _rb_define_module_function(mod, "remove", cosmosFastSpatialRemove);
    _rb_define_module_function(mod, "query_radius", cosmosFastSpatialQueryRadius);
    _rb_define_module_function(mod, "nearest", cosmosFastSpatialNearest);
    _rb_define_module_function(mod, "stats", cosmosFastSpatialStats);

    VALUE version = rb_utf8_str_new_cstr("1.0");
    rb_str_freeze(version);
    rb_define_const(mod, "VERSION", version);

    rb_define_const(mod, "TEAM_ALLY", INT2NUM(TEAM_ALLY));
    rb_define_const(mod, "TEAM_ENEMY", INT2NUM(TEAM_ENEMY));
    rb_define_const(mod, "TEAM_NEUTRAL", INT2NUM(TEAM_NEUTRAL));

    rb_define_const(mod, "KIND_CHAMPION", INT2NUM(KIND_CHAMPION));
    rb_define_const(mod, "KIND_MINION", INT2NUM(KIND_MINION));
    rb_define_const(mod, "KIND_PET", INT2NUM(KIND_PET));
    rb_define_const(mod, "KIND_OTHER", INT2NUM(KIND_OTHER));
}
