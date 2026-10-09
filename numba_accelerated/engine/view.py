import pygame, sys, time, math

CELL = 15

PLAYERC = [
    (255, 0, 0),
    (0, 0, 255),
    (0, 255, 0),
    (255, 255, 0),
    (0, 255, 255),
    (255, 0, 255),
    (255, 127, 0),
    (255, 127, 127),
    (128, 0, 0),
    (0, 0, 128),
    (0, 128, 0),
    (128, 128, 0),
    (0, 128, 128),
    (128, 0, 128),
    (128, 127, 0),
    (128, 127, 127),
]

class GameView(object):
    def __init__(self, game=None , fast=False):
        pygame.init()
        pygame.display.set_caption("aicontest")
        self.font = pygame.font.Font("freesansbold.ttf", 22)
        self.scale = 1
        if fast:
            size = self.width, self.height = 1, 1
        else:
            size = self.width, self.height = 1440, 800
            self.scale = 2
        self.screen = pygame.display.set_mode(size, pygame.DOUBLEBUF)
        if game:
            self.attach(game)

    def attach(self, game):
        self.game = game
        self.fw = self.game.island.w * CELL * self.scale
        self.fh = self.game.island.h * CELL * self.scale
        self.arena = pygame.Surface((self.fw, self.fh), 0, self.screen)
        self.nh = self.game.island.h - 1

    def _afill(self, xxx_todo_changeme, xxx_todo_changeme1, c):
        (x0, y0) = xxx_todo_changeme
        (w, h) = xxx_todo_changeme1
        x0 *= self.scale
        y0 *= self.scale
        w *= self.scale
        h *= self.scale
        self.arena.fill(c, (x0, y0, w, h))

    def _aaline(self, xxx_todo_changeme2, xxx_todo_changeme3, c):
        (x0, y0) = xxx_todo_changeme2
        (x1, y1) = xxx_todo_changeme3
        x0 *= self.scale
        y0 *= self.scale
        x1 *= self.scale
        y1 *= self.scale
        for i in range(self.scale):
            pygame.draw.aaline(self.arena, c, (x0+i, y0+i), (x1+i, y1+i))

    def _diamond(self, xxx_todo_changeme4, size, c, width=0):
        (cx, cy) = xxx_todo_changeme4
        cx *= self.scale
        cy *= self.scale
        size *= self.scale
        points = [
            (cx - size, cy),
            (cx, cy - size),
            (cx + size, cy),
            (cx, cy + size),
        ]
        pygame.draw.polygon(self.arena, c, points, width)

    def cmul(self, xxx_todo_changeme5, mul):
        (r, g, b) = xxx_todo_changeme5
        return int(r * mul), int(g * mul), int(b * mul)

    def calpha(self, xxx_todo_changeme6, xxx_todo_changeme7, a):
        (r1, g1, b1) = xxx_todo_changeme6
        (r2, g2, b2) = xxx_todo_changeme7
        return (int(r2 * a + r1 * (1-a)),
                int(g2 * a + g1 * (1-a)),
                int(b2 * a + b1 * (1-a)))

    def draw_cell(self, xxx_todo_changeme8):
        (cx, cy) = xxx_todo_changeme8
        py = (self.nh - cy) * CELL
        px = cx * CELL
        c = int(self.game.island.energy[cx, cy] / 100.0 * 25)
        bg = tuple(map(int,(25+c*0.8, 25+c*0.8, 25+c)))

        for vertices, fill in self.game.tris.items():
            if (cx, cy) in fill:
                owner = self.game.lighthouses[vertices[0]].owner
                bg = self.calpha(bg, PLAYERC[owner], 0.15)

        self._afill((px, py), (CELL, CELL), bg)
        self._afill((px + CELL/2, py + CELL/2), (1,1), (255,255,255))

        cplayers = [i for i in self.game.players if i.pos == (cx, cy)]
        if cplayers:
            nx = int(math.ceil(math.sqrt(len(cplayers))))
            wx = 12 / nx
            ny = int(math.ceil(len(cplayers)/float(nx)))
            wy = 12 / ny
            for i, player in enumerate(cplayers):
                iy = i / nx
                ix = i % nx
                color = self.cmul(PLAYERC[player.num], 0.5)
                self._afill((px + 2 + ix * wx, py + 2 + iy * wy),
                            (wx - 1, wy - 1), color)

        if (cx, cy) in self.game.lighthouses:
            lh = self.game.lighthouses[cx, cy]
            color = (192, 192, 192)
            if lh.owner is not None:
                color = PLAYERC[lh.owner]
            self._diamond((px + CELL/2, py + CELL/2), 4, color, 0)

    def update(self):
        self.arena.fill((0, 0, 0))
        for cy in range(self.game.island.h):
            for cx in range(self.game.island.w):
                if self.game.island[cx, cy]:
                    self.draw_cell((cx, cy))
        for (x0, y0), (x1, y1) in self.game.conns:
            owner = self.game.lighthouses[x0, y0].owner
            color = PLAYERC[owner]
            y0, y1 = self.nh - y0, self.nh - y1
            self._aaline((x0 * CELL + CELL/2, y0 * CELL + CELL/2),
                        (x1 * CELL + CELL/2, y1 * CELL + CELL/2), color)
        self.screen.fill((0, 0, 0))
        self.screen.blit(self.arena, (0,0))
        y = 20
        x = 1200

        for p in self.game.players:
            fr = self.font.render("%s: %d (%d)" % (p.name, p.score, p.score+p.cumscore), False, PLAYERC[p.num])
            self.screen.blit(fr, (x,y))
            y += 40
        fr = self.font.render(self.title, False, (255,255,255))
        self.screen.blit(fr, (10,self.height-40))
        pygame.display.flip()

    def try_sort_by_score(self, players):
        if players:
            players.sort(key=lambda player: player.score + player.cumscore, reverse=True)
            return players
        return []
