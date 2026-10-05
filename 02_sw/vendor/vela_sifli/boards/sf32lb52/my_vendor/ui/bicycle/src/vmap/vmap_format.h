/**
 * @file vmap_format.h
 * @brief 矢量地图二进制格式常量（与 tools/pack_vmap.py 及设备端读取器共享）。
 */

#ifndef VMAP_FORMAT_H
#define VMAP_FORMAT_H

#include <stdint.h>

#define VMAP_MAGIC0 'V'
#define VMAP_MAGIC1 'T'
#define VMAP_MAGIC2 'I'
#define VMAP_MAGIC3 'L'

#define VMAP_PACK_MAGIC0 'V'
#define VMAP_PACK_MAGIC1 'P'
#define VMAP_PACK_MAGIC2 'K'
#define VMAP_PACK_MAGIC3 'G'

#define VMAP_VERSION 2
#define VMAP_PACK_VERSION 1

#define VMAP_PACK_TARGET_BYTES (500U * 1024U)

/* <map>/map.idx — unified tile + graph shard table (VIDX, pack_map.py). */
#define VMAP_MAP_INDEX_MAGIC0 'V'
#define VMAP_MAP_INDEX_MAGIC1 'I'
#define VMAP_MAP_INDEX_MAGIC2 'D'
#define VMAP_MAP_INDEX_MAGIC3 'X'
#define VMAP_MAP_INDEX_VERSION 1
#define VMAP_MAP_INDEX_HDR_SIZE 24
#define VMAP_MAP_INDEX_TILE_REC_SIZE 14
#define VMAP_MAP_INDEX_GRAPH_REC_SIZE 10
#define VMAP_MAP_FLAG_HAS_GRAPH 0x01

/* map.idx — VREG v1 regional catalog (pack_map.py --region-km). */
#define VMAP_REG_INDEX_MAGIC0 'V'
#define VMAP_REG_INDEX_MAGIC1 'R'
#define VMAP_REG_INDEX_MAGIC2 'E'
#define VMAP_REG_INDEX_MAGIC3 'G'
#define VMAP_REG_INDEX_VERSION 1
#define VMAP_REG_INDEX_HDR_SIZE 40
#define VMAP_REG_INDEX_REC_SIZE 48

/* Inside rNNN.vpk payload — per-region tile table + graph blob. */
#define VMAP_REG_PACK_INDEX_MAGIC0 'R'
#define VMAP_REG_PACK_INDEX_MAGIC1 'I'
#define VMAP_REG_PACK_INDEX_MAGIC2 'D'
#define VMAP_REG_PACK_INDEX_MAGIC3 'X'
#define VMAP_REG_PACK_INDEX_VERSION 1
#define VMAP_REG_PACK_INDEX_VERSION2 2
#define VMAP_REG_PACK_INDEX_HDR_SIZE 52
#define VMAP_REG_PACK_TILE_REC_SIZE 14
#define VMAP_REG_PACK_TILE_REC_SIZE_V2 16
/* VTIL / RIDX tile payload cap. uint16 could not store this, so RIDX v2
 * uses a uint32 size field. Larger than this is a pack error. */
#define VMAP_TILE_BYTES_MAX (128u * 1024u)

/* Coordinate-addressed cells: no map.idx.
 *   <map>/lon<ix/BUCKET>/lat<iy/BUCKET>/x<ix>_y<iy>.vpk
 * ix = floor((lon - origin_lon) / cell_lon), iy likewise.
 * Keep these numbers in sync with pack_map.py. */
#define VMAP_GRID_ORIGIN_LON      0.0
#define VMAP_GRID_ORIGIN_LAT      0.0
#define VMAP_GRID_ANCHOR_LAT      35.0
#define VMAP_GRID_REGION_KM       3.0
#define VMAP_GRID_CELL_LAT        0.026949335249730506  /* km / 111.32 */
#define VMAP_GRID_CELL_LON        0.032899063849730509  /* km / (111.32*cos(35°)) */
#define VMAP_GRID_BUCKET          4u
#define VMAP_GRID_BUCKET_LON      "lon"
#define VMAP_GRID_BUCKET_LAT      "lat"
#define VMAP_GRID_PATH_MAX        128

typedef uint32_t vmap_cell_id_t;
#define VMAP_CELL_NONE            0xffffffffu
#define VMAP_CELL_ID(ix, iy)      ((((vmap_cell_id_t)(uint16_t)(iy)) << 16) \
                                   | (vmap_cell_id_t)(uint16_t)(ix))
#define VMAP_CELL_IX(id)          ((uint16_t)(id))
#define VMAP_CELL_IY(id)          ((uint16_t)((id) >> 16))

/* Legacy MTP layout (rNNN / dNNN) — no longer written. */
#define VMAP_REG_SUBDIR_PREFIX    "d"
#define VMAP_REG_FILES_PER_DIR    20
#define VMAP_REG_VPK_PREFIX       "r"

/* Legacy per-zoom tiles.idx (VTIX) — goldfish / old packs only. */
#define VMAP_TILES_INDEX_MAGIC0 'V'
#define VMAP_TILES_INDEX_MAGIC1 'T'
#define VMAP_TILES_INDEX_MAGIC2 'I'
#define VMAP_TILES_INDEX_MAGIC3 'X'
#define VMAP_TILES_INDEX_VERSION 1

#define VMAP_INDEX_MAGIC0 VMAP_TILES_INDEX_MAGIC0
#define VMAP_INDEX_MAGIC1 VMAP_TILES_INDEX_MAGIC1
#define VMAP_INDEX_MAGIC2 VMAP_TILES_INDEX_MAGIC2
#define VMAP_INDEX_MAGIC3 VMAP_TILES_INDEX_MAGIC3
#define VMAP_INDEX_VERSION VMAP_TILES_INDEX_VERSION
#define VMAP_VPK_HDR_SIZE 16

#define VMAP_EXTENT 4096

#define VMAP_LAYER_WATER 0
#define VMAP_LAYER_FOREST 1
#define VMAP_LAYER_WATERWAY 2
#define VMAP_LAYER_ROAD 3
#define VMAP_LAYER_LABEL 4
#define VMAP_LAYER_LAND 5
#define VMAP_LAYER_MAX 6

#define VMAP_LAND_RESIDENTIAL 1
#define VMAP_LAND_COMMERCIAL  2
#define VMAP_LAND_INDUSTRIAL  3
#define VMAP_LAND_EDU         4
#define VMAP_LAND_PARK        5

#define VMAP_LABEL_PLACE 1
#define VMAP_LABEL_WATER 2
#define VMAP_LABEL_ROAD 3
#define VMAP_LABEL_POI 4
#define VMAP_LABEL_MAXLEN 60

#define VMAP_FLAG_CLOSED 0x01
#define VMAP_FLAG_NAMED 0x02

#define VMAP_ROAD_MOTORWAY 1
#define VMAP_ROAD_PRIMARY 2
#define VMAP_ROAD_SECONDARY 3
#define VMAP_ROAD_TERTIARY 4
#define VMAP_ROAD_RESIDENTIAL 5
#define VMAP_ROAD_SERVICE 6
#define VMAP_ROAD_PATH 7

/* VGRF — offline routing graph (see docs/osm/ROUTING.md) */
#define VGRF_MAGIC0 'V'
#define VGRF_MAGIC1 'G'
#define VGRF_MAGIC2 'R'
#define VGRF_MAGIC3 'F'

#define VGRF_VERSION 4
#define VGRF_PROFILE_BICYCLE 0

/* v1: 48 B (no snap_off). v2+: 52 B (+ snap_off @48). Nodes follow header. */
#define VGRF_HDR_SIZE_V1 48
#define VGRF_HDR_SIZE_V2 52
#define VGRF_HDR_SNAP_OFF 48

#define VGRF_EDGE_ONEWAY 0x01
#define VGRF_EDGE_NO_BIKE 0x02

/* v1/v2 edge: 16 B, name_off uint16, 0xFFFF = none.
 * v3/v4 edge: 18 B, name_off uint32, 0xFFFFFFFF = none.
 * v4: undirected (A–B stored once; ONEWAY means only from→to). No adj tables;
 * the device builds outgoing adjacency on load. v2/v3 keep directed + adj. */
#define VGRF_EDGE_SIZE_V2 16
#define VGRF_EDGE_SIZE_V3 18
#define VGRF_NAME_NONE_V2 0xFFFFu
#define VGRF_NAME_NONE 0xFFFFFFFFu

/* Device-side string pool cap (clipped 15 km region). Host city-wide graphs
 * may be larger; pack_map re-interns per region and must stay within this. */
#define VGRF_STR_POOL_MAX (128u * 1024u)

#define VGRF_SNAP_MAGIC0 'S'
#define VGRF_SNAP_MAGIC1 'N'
#define VGRF_SNAP_MAGIC2 'A'
#define VGRF_SNAP_MAGIC3 'P'
#define VGRF_SNAP_VERSION 1
#define VGRF_SNAP_HDR_SIZE 20

/* Optional trailing section after SNAP: per-node elevation in metres.
 * INT16_MIN (-32768) means unknown. Old firmware ignores bytes past SNAP. */
#define VGRF_ELEV_MAGIC0 'E'
#define VGRF_ELEV_MAGIC1 'L'
#define VGRF_ELEV_MAGIC2 'E'
#define VGRF_ELEV_MAGIC3 'V'
#define VGRF_ELEV_VERSION 1
#define VGRF_ELEV_HDR_SIZE 12
#define VGRF_ELE_UNKNOWN (-32768)

/* route.port — precomputed cross-region portals (pack_map.py). */
#define VMAP_ROUTE_PORT_MAGIC0 'P'
#define VMAP_ROUTE_PORT_MAGIC1 'O'
#define VMAP_ROUTE_PORT_MAGIC2 'R'
#define VMAP_ROUTE_PORT_MAGIC3 'T'
#define VMAP_ROUTE_PORT_VERSION 1
#define VMAP_ROUTE_PORT_HDR_SIZE 10
#define VMAP_ROUTE_PORT_REC_SIZE 28
#define VMAP_ROUTE_PORT_FILE     "route.port"

/* route.port v2: sorted per-region-pair index + on-disk record blocks. The
 * device keeps only the compact pair index in RAM and reads a pair's records
 * on demand (v1 = flat records, full-load fallback). */
#define VMAP_ROUTE_PORT_VERSION2   2
#define VMAP_ROUTE_PORT_V2_HDR_SIZE 14
#define VMAP_ROUTE_PORT_PAIR_SIZE   12

/* v3: cell_id is uint32 (iy<<16|ix). Pair index stays on disk (binary search). */
#define VMAP_ROUTE_PORT_VERSION3      3
#define VMAP_ROUTE_PORT_V3_HDR_SIZE   14
#define VMAP_ROUTE_PORT_PAIR_SIZE_V3  16
#define VMAP_ROUTE_PORT_REC_SIZE_V3   32

/* Trailing portal table in a cell vpk, immediately after the VGRF blob.
 * Tile and graph readers ignore it (they use graph_off / graph_size).
 * New packs put portals here instead of a top-level route.port. */
#define VMAP_VPOR_MAGIC0     'V'
#define VMAP_VPOR_MAGIC1     'P'
#define VMAP_VPOR_MAGIC2     'O'
#define VMAP_VPOR_MAGIC3     'R'
#define VMAP_VPOR_VERSION    1
#define VMAP_VPOR_HDR_SIZE   8
#define VMAP_VPOR_NBR_SIZE   8
#define VMAP_VPOR_REC_SIZE   24
#define VMAP_VPOR_NBR_MAX    16

#endif /* VMAP_FORMAT_H */
