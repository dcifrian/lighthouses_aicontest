#!/usr/bin/python

import numba 
from numba import types
from numba.experimental import jitclass
import numpy as np

EnergyType = numba.types.deferred_type()
IslandType = numba.types.deferred_type()

@jitclass([('maxEnergy', numba.int64), 
           ('energy', EnergyType),
           ('_energyMap', numba.int64[:,:])])
class Island:

    def __init__(self, maxEnergy):
        self.maxEnergy = maxEnergy
        self.energy = Energy(self)
        self._energyMap = np.zeros((2,2), dtype=np.int64) 

    def getMaxEnergy(self):
        return self.maxEnergy
    
    def getEnergyMap(self):
        return self._energyMap

    def setEnergyMap(self, map):
        self._energyMap = map

@jitclass([('island', IslandType)])
class Energy:

    def __init__(self, island):
        self.island = island

    def __getitem__(self, pos):
        x, y = pos
        if self.island.getMaxEnergy() < self.island.getEnergyMap()[y][x]:
            return self.island.getEnergyMap()[y][x]
        else:
            return 0

    def __setitem__(self, pos, val):
        x, y = pos
        if val > self.island.getMaxEnergy():
            val = self.island.getMaxEnergy()
        map = self.island.getEnergyMap()
        map[y][x] = val
        self.island.setEnergyMap(map)

IslandType.define(Island.class_type.instance_type)
EnergyType.define(Energy.class_type.instance_type)

#IslandType.class_type.instance_type = IslandType
#EnergyType.class_type.instance_type = EnergyType

island = Island(100)
print(island.energy[1,0])
island.energy[1,0] = 150  
print(island.energy[1,0])
