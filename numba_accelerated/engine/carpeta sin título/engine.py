#!/usr/bin/python

import geom, math
#from FrozenSetNumba import FrozenSetNumba
from numba import jit
from numba.experimental import jitclass
from numba import boolean, int32, int64, float32, int8, types, typed, typeof, deferred_type,objmode
from numba.extending import as_numba_type

class MoveError(Exception):
    pass

class GameError(Exception):
    pass


islandTypeDeferred = types.deferred_type()
energyTypeDeferred = types.deferred_type()
gameTypeDeferred = types.deferred_type()
playerTypeDeferred = types.deferred_type()
lighthouseTypeDeferred = types.deferred_type()

int64ListType=types.ListType(int64) #numba wants the types of a nested list declared and stored in a variable, inlining the call fails with a non descriptive error.
float32ListType=types.ListType(float32)


islandSpec = [
    ('MAX_ENERGY', int64),
    ('HORIZON', int64),        
    ('_island', types.ListType(int64ListType)),        
    ('h', int64),        
    ('w', int64),        
    ('_energymap',types.ListType(int64ListType)),        
    ('_horizonmap',types.ListType(float32ListType)),
    ('_energy',energyTypeDeferred)
]

@jitclass(islandSpec)
class Island(object):   
    def __init__(self, island_map):
        self.MAX_ENERGY = 100
        self.HORIZON = 3       
        self._island = island_map
        #self._island = typed.List.empty_list(int64ListType)
        #for sublist in island_map:
        #    typed_sublist = typed.List.empty_list(int64)
        #    for item in sublist:
        #        typed_sublist.append(0)
        #self._island.append(typed_sublist)
        #self._island = [[0] * (self.w+100) for i in range(self.h+100)]
        self.h = len(self._island)
        self.w = len(self._island[0])
        self._energymap = typed.List.empty_list(int64ListType)
        for i in range(self.h+1):
            l = typed.List.empty_list(int64)
            for j in range(self.w+1):
                l.append(0)
            self._energymap.append(l)
        self._horizonmap = typed.List.empty_list(float32ListType)
        dist = self.HORIZON
        for y in range(-dist, dist + 1):
            row = typed.List.empty_list(float32)
            for x in range(-dist, dist + 1):
                row.append(geom.dist((0,0), (x,y)) <= self.HORIZON)
            self._horizonmap.append(row)              
        self._energy = _Energy(self)            

    def __getitem__(self, pos):
        x, y = pos
        if 0 <= x < self.w and 0 <= y < self.h:
            return self._island[y][x]
        else:
            return False

    @property
    def energy(self):
        return self._energy

    def get_energy(self,pos):
        x, y = pos
        if self[pos]:            
            return self._energymap[y][x]
        else:
            return 0

    def set_energy(self,pos, val):
        x, y = pos
        if val > self.MAX_ENERGY:
            val = self.MAX_ENERGY
        assert val >= 0
        if self[pos]:
            self._energymap[y][x] = val

    def get_view(self, pos):
        px, py = pos
        dist = self.HORIZON
        view = []
        for y in range(-dist, dist + 1):
            row = []
            for x in range(-dist, dist + 1):
                if self._horizonmap[y+dist][x+dist]:
                    #row.append(self.energy[px+x, py+y])
                    row.append(self.get_energy([px+x, py+y])) # Numba seems to have trouble figuring out the type through the property
                else:
                    row.append(-1)
            view.append(row)
        return view

    @property
    def map(self):
        return self._island

@jitclass([ ('_island',islandTypeDeferred) ])
class _Energy(object):
    def __init__(self,island):
        self._island=island

    def __getitem__(self, pos):
        return self._island.get_energy(pos)

    def __setitem__(self, pos, val):
        self._island.set_energy(pos,val)

energyTypeDeferred.define(_Energy.class_type.instance_type)
islandTypeDeferred.define(Island.class_type.instance_type)

#@jitclass([('game',gameTypeDeferred),('pos',types.UniTuple(int64,2)),('owner',types.optional(int64)),('energy',int64)])
class Lighthouse(object):
    def __init__(self, game, pos):
        self.game = game
        self.pos = pos
        self.owner = None
        self.energy = 0

    def attack(self, player, strength):
        if not isinstance(strength, int):
            raise MoveError("Strength must be an int")
        if strength < 0:
            raise MoveError("Strength must be positive")
        if strength > player.energy:
            strength = player.energy
        player.energy -= strength
        if self.owner is not None and self.owner != player.num:
            d = min(self.energy, strength)
            self.decay(d)
            strength -= d
        if strength:
            self.owner = player.num
            self.energy += strength

    def decay(self, by):
        self.energy -= by
        if self.energy <= 0:
            self.energy = 0
            self.owner = None
            self.game.conns = set(i for i in self.game.conns if self.pos not in i)
            self.game.tris = dict(i for i in self.game.tris.items() if self.pos not in i[0])

#@jitclass([('num',int64),('game',gameTypeDeferred),('pos',typeof((0,0))),('score',int64),('energy',int64),('keys',types.Set(types.UniTuple(int64,2),reflected=True)),('name',types.unicode_type)])
class Player(object):
    def __init__(self, game, num, init_pos):
        self.num = num
        self.game = game
        self.pos = init_pos
        self.score = 0
        self.energy = 0
        self.keys = set()
        self.name = "Player %d" % num

    def move(self, delta):
        dx, dy = delta
        if dx not in (0, 1, -1) or dy not in (0, 1, -1):
            raise MoveError("Delta must be 1 cell away")
        new_pos = self.pos[0] + dx, self.pos[1] + dy
        if not self.game.island[new_pos]:
            raise MoveError("Target pos is not in island")
        self.pos = new_pos

def readMapFile(mapfile):
    lines = typed.List.empty_list(types.unicode_type)
    with open(mapfile, "r") as fd:
        [lines.append(l.replace("\n", "")) for l in fd.readlines()]
    return lines

@jitclass([('lighthouses',typeof([(0,0)])),('island',types.ListType(int64ListType)),('players',typeof([(0,0)]))])
class GameConfig(object):
    def __init__(self, mapfile):
        with objmode(lines='types.ListType(types.unicode_type)'):  # annotate return type
            lines = readMapFile(mapfile)
        self.lighthouses = [(0,0) for x in range(0)]
        players = [(" ", (0,0)) for x in range(0)]
        self.island = typed.List.empty_list(int64ListType)
        for y, line in enumerate(lines[::-1]):
            row = typed.List.empty_list(int64)
            for x, c in enumerate(line):
                if c == "#":
                    row.append(0)
                elif c == "!":
                    row.append(1)
                    self.lighthouses.append((x,y))
                elif c == " ":
                    row.append(1)
                else:
                    row.append(1)
                    players.append((c, (x,y)))
            self.island.append(row)
        self.players = [pos for c, pos in sorted(players)]
        w = len(self.island[0])
        h = len(self.island)
        #if not all(len(l) == w for l in self.island):
        #    raise GameError("All map rows must have the same width")
        #if (not all(not i for i in self.island[0]) or
        #    not all(not i for i in self.island[-1]) or
        #    not all(not (i[0] or i[-1]) for i in self.island)):
        #    raise GameError("Map border must not be part of island")


#frozenConnType=typeof(frozenset(((0,0),(0,0))))
#frozenConnType=types.Hashable(FrozenSetNumba.class_type.instance_type)
"""
frozenConnType=types.Hashable(typeof( FrozenSetNumba( ((0,0),(0,0)) ) ) )

GameSpec=[
    ('RDIST',int64),
    ('island',islandTypeDeferred),
    ('lighthouses',types.DictType(int64,lighthouseTypeDeferred)),
    ('conns',types.Set(frozenConnType,reflected=False)),
    ('tris',types.DictType(types.UniTuple(types.UniTuple(int64,2),3))),
    ('players',playerTypeDeferred[:])
]
#conns es un set de frozenset((orig.pos, dest.pos))

@jitclass(GameSpec)
"""
class Game(object):
    def __init__(self, cfg, numplayers=None):
        self.RDIST = 5
        if numplayers is None:
            numplayers = len(cfg.players)
        assert numplayers <= len(cfg.players)
        self.island = Island(cfg.island)
        #self.lighthouses = dict((x, Lighthouse(self, x)) for x in cfg.lighthouses)
        self.lighthouses = dict()
        for x in cfg.lighthouses:
            self.lighthouses[x] = Lighthouse(self, x)
        self.conns = set()
        self.tris = dict()
        self.players = [Player(self, i, pos) for i, pos in enumerate(cfg.players[:numplayers])]

    def connect(self, player, dest_pos):
        if player.pos not in self.lighthouses:
            raise MoveError("Player must be located at the origin lighthouse")
        if dest_pos not in self.lighthouses:
            raise MoveError("Destination must be an existing lighthouse")
        orig = self.lighthouses[player.pos]
        dest = self.lighthouses[dest_pos]
        if orig.owner != player.num or dest.owner != player.num:
            raise MoveError("Both lighthouses must be player-owned")
        if dest.pos not in player.keys:
            raise MoveError("Player does not have the destination key")
        if orig is dest:
            raise MoveError("Cannot connect lighthouse to itself")
        assert orig.energy and dest.energy
        pair = frozenset((orig.pos, dest.pos))
        if pair in self.conns:
            raise MoveError("Connection already exists")
        x0, x1 = sorted((orig.pos[0], dest.pos[0]))
        y0, y1 = sorted((orig.pos[1], dest.pos[1]))
        for lh in self.lighthouses:
            if (x0 <= lh[0] <= x1 and y0 <= lh[1] <= y1 and
                lh not in (orig.pos, dest.pos) and
                geom.colinear(orig.pos, dest.pos, lh)):
                raise MoveError("Connection cannot intersect a lighthouse")
        new_tris = set()
        for c in self.conns:
            if geom.intersect(tuple(c), (orig.pos, dest.pos)):
                raise MoveError("Connection cannot intersect another connection")
            if orig.pos in c:
                third = next((l for l in c if l != orig.pos))
                if frozenset((third, dest.pos)) in self.conns:
                    new_tris.add((orig.pos, dest.pos, third))

        player.keys.remove(dest.pos)
        self.conns.add(pair)
        for i in new_tris:
            self.tris[i] = [j for j in geom.render(i) if self.island[j]]
    
    def pre_round(self):
        for pos in self.lighthouses:
            for y in range(pos[1]-self.RDIST+1, pos[1]+self.RDIST):
                for x in range(pos[0]-self.RDIST+1, pos[0]+self.RDIST):
                    dist = geom.dist(pos, (x,y))
                    delta = int(math.floor(self.RDIST - dist))
                    if delta > 0:
                        self.island.energy[x,y] += delta
        player_posmap = dict()
        for player in self.players:
            if player.pos in player_posmap:
                player_posmap[player.pos].append(player)
            else:
                player_posmap[player.pos] = [player]
            if player.pos in self.lighthouses:
                player.keys.add(player.pos)
        for pos, players in player_posmap.items():
            energy = self.island.energy[pos] // len(players)
            for player in players:
                player.energy += energy
            self.island.energy[pos] = 0
        for lh in self.lighthouses.values():
            lh.decay(10)
    
    def post_round(self):
        for lh in self.lighthouses.values():
            if lh.owner is not None:
                self.players[lh.owner].score += 2
        for pair in self.conns:
            self.players[self.lighthouses[next(iter(pair))].owner].score += 2
        for tri, cells in self.tris.items():
            self.players[self.lighthouses[tri[0]].owner].score += len(cells)


#gameTypeDeferred.define(Game.class_type.instance_type)
#playerTypeDeferred.define(Player.class_type.instance_type)
#lighthouseTypeDeferred.define(Lighthouse.class_type.instance_type)


