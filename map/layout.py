"""Lay out screenshot cards and route door links through the gaps between them."""

import heapq
import math
import random
from collections import Counter, defaultdict


GRID = 16
CARD_W, CARD_H, IMAGE_Y = 256, 192, 54
PLAY_H = CARD_H - IMAGE_Y
CELL_X, CELL_Y = 368, 288
MARGIN = 48
CROSSING_COST = 115
SIDE = {"left": (-1, 0), "right": (1, 0), "top": (0, -1), "bottom": (0, 1)}


def door_side(door):
    x = (door["x"] + 6) * CARD_W / 160
    y = (door["y"] + 12) * PLAY_H / 144
    distances = {"left": x, "right": CARD_W - x, "top": y + 30, "bottom": PLAY_H - y}
    return min(distances, key=distances.get)


def edge_cost(link, placed):
    start, end, start_direction, end_direction = link
    ax, ay = placed[start]
    bx, by = placed[end]
    dx, dy = bx - ax, by - ay
    distance = math.hypot(dx, dy * 0.8)
    away = dx * start_direction[0] + dy * start_direction[1]
    toward = -dx * end_direction[0] - dy * end_direction[1]
    return distance ** 1.23 + 0.72 * (max(0, 1 - away) + max(0, 1 - toward))


def place_rooms(room_ids, links, gate=None, gate_side=0):
    columns = max(1, math.ceil(math.sqrt(len(room_ids) * 1.28)))
    rows = max(1, math.ceil(len(room_ids) / columns) + 1)
    positions = [(x, y) for y in range(rows) for x in range(columns)]
    center = (columns - 1) / 2, (rows - 1) / 2
    adjacency = defaultdict(list)
    for index, (start, end, *_) in enumerate(links):
        if start != end:
            adjacency[start].append(index)
            adjacency[end].append(index)

    def centrality(position):
        return 0.026 * math.dist(position, center) ** 1.4

    def position_cost(room, position):
        cost = centrality(position)
        if room == gate:
            edge = 0 if gate_side < 0 else columns - 1
            cost += 2 * abs(position[0] - edge)
        return cost

    best = {}
    best_cost = float("inf")
    for seed in range(5):
        rng = random.Random(341 + seed)
        placed = {}
        occupants = {}
        pending = set(room_ids)
        while pending:
            room = min(pending, key=lambda key: (
                -sum(any(other in placed for other in links[i][:2]) for i in adjacency[key]), key))
            placed_links = [i for i in adjacency[room] if any(other in placed for other in links[i][:2])]

            def rank(position):
                placed[room] = position
                cost = sum(edge_cost(links[i], placed) for i in placed_links)
                return cost + position_cost(room, position) + rng.random() * (0.08 + seed * 0.17)

            spot = min((position for position in positions if position not in occupants), key=rank)
            placed[room] = spot
            occupants[spot] = room
            pending.remove(room)

        iterations = max(2400, len(room_ids) * 360)
        for step in range(iterations):
            start = rng.choice(room_ids)
            previous = placed[start]
            proposed = rng.choice(positions)
            if previous == proposed:
                continue
            end = occupants.get(proposed)
            relevant = set(adjacency[start])
            if end is not None:
                relevant.update(adjacency[end])
            before = sum(edge_cost(links[i], placed) for i in relevant)
            placed[start] = proposed
            if end is not None:
                placed[end] = previous
            after = sum(edge_cost(links[i], placed) for i in relevant)
            after += position_cost(start, proposed) - position_cost(start, previous)
            if end is not None:
                after += position_cost(end, previous) - position_cost(end, proposed)
            heat = 0.92 * 0.015 ** (step / iterations)
            if after <= before or rng.random() < math.exp((before - after) / heat):
                occupants[proposed] = start
                if end is None:
                    del occupants[previous]
                else:
                    occupants[previous] = end
            else:
                placed[start] = previous
                if end is not None:
                    placed[end] = proposed

        cost = sum(edge_cost(link, placed) for link in links)
        cost += sum(position_cost(room, position) for room, position in placed.items())
        if cost < best_cost:
            best, best_cost = placed, cost

    left = min(x for x, _ in best.values())
    top = min(y for _, y in best.values())
    return {key: (MARGIN + (x - left) * CELL_X, MARGIN + (y - top) * CELL_Y)
            for key, (x, y) in best.items()}


def door_anchor(door, position):
    left, top = position
    return (round(left + (door["x"] + 6) * CARD_W / 160, 1),
            round(top + IMAGE_Y + (door["y"] + 12) * PLAY_H / 144, 1))


def align_neighbors(positions, doors, connections):
    candidates = []
    for edge in connections:
        room_a, door_a = doors[edge["from"]]
        room_b, door_b = doors[edge["to"]]
        direction = SIDE[door_side(door_a)]
        if SIDE[door_side(door_b)] != (-direction[0], -direction[1]):
            continue
        axis = 0 if direction[0] else 1
        across = 1 - axis
        first, second = positions[room_a], positions[room_b]
        distance = (second[axis] - first[axis]) * direction[axis]
        if not 0 < distance <= (CELL_X, CELL_Y)[axis] * 1.5:
            continue
        delta = door_anchor(door_a, first)[across] - door_anchor(door_b, second)[across]
        if abs(delta) <= 25:
            candidates.append((abs(delta), distance, edge["from"], edge["to"], across))

    locked = set()
    shifted = {room: list(point) for room, point in positions.items()}
    for _, _, start, end, axis in sorted(candidates):
        first, door_a = doors[start]
        second, door_b = doors[end]
        if (first, axis) in locked and (second, axis) in locked:
            continue
        delta = door_anchor(door_a, shifted[first])[axis] - door_anchor(door_b, shifted[second])[axis]
        left = 0 if (first, axis) in locked else -delta if (second, axis) in locked else round(-delta / 2, 1)
        right = delta + left
        if abs(shifted[first][axis] + left - positions[first][axis]) > 25 or abs(shifted[second][axis] + right - positions[second][axis]) > 25:
            continue
        shifted[first][axis] = round(shifted[first][axis] + left, 1)
        shifted[second][axis] = round(shifted[second][axis] + right, 1)
        locked.update(((first, axis), (second, axis)))
    return shifted


def door_points(door, position):
    x, y = door_anchor(door, position)
    left, top = position
    dx, dy = SIDE[door_side(door)]
    port = (left - GRID if dx < 0 else left + CARD_W + GRID if dx > 0 else x,
            top - GRID if dy < 0 else top + CARD_H + GRID if dy > 0 else y)
    return (x, y), tuple(round(value, 1) for value in port), (dx, dy)


def lane_keys(first, second):
    axis = 0 if first[1] == second[1] else 1
    start, end = sorted((first[axis], second[axis]))
    lane = round(first[1 - axis] / GRID)
    return [(axis, lane, cell) for cell in range(int(start // GRID), math.ceil(end / GRID))]


def route(start, end, bounds, positions, used_edges, crossings, start_direction, end_direction):
    axes = []
    for axis, limit in enumerate(bounds):
        anchors = {start[axis], end[axis]}
        lanes = {coordinate for coordinate in range(0, limit + 1, GRID)
                 if all(abs(coordinate - anchor) >= GRID / 2 for anchor in anchors)}
        axes.append(sorted(anchors | lanes))
    source = tuple(axes[i].index(start[i]) for i in range(2))
    destination = tuple(axes[i].index(end[i]) for i in range(2))
    blocked = set()
    for x, y in positions.values():
        blocked.update((i, j) for i, cx in enumerate(axes[0]) if x - 5 <= cx <= x + CARD_W + 5
                       for j, cy in enumerate(axes[1]) if y - 5 <= cy <= y + CARD_H + 5)
    directions = tuple(SIDE.values())
    first_direction = directions.index(start_direction)
    last_direction = directions.index(tuple(-value for value in end_direction))
    queue = [(0, 0, source, first_direction)]
    state = (source, first_direction)
    costs = {state: 0}
    parents = {}
    while queue:
        _, cost, point, direction = heapq.heappop(queue)
        state = point, direction
        if cost != costs.get(state):
            continue
        if point == destination and direction == last_direction:
            trail = [point]
            while state in parents:
                state = parents[state]
                trail.append(state[0])
            return [(axes[0][i], axes[1][j]) for i, j in reversed(trail)]
        for index, (dx, dy) in enumerate(directions):
            next_point = point[0] + dx, point[1] + dy
            x, y = next_point
            if (dx, dy) == tuple(-value for value in directions[direction]):
                continue
            if not (0 <= x < len(axes[0]) and 0 <= y < len(axes[1])) or next_point in blocked:
                continue
            if next_point == destination and index != last_direction:
                continue
            here = (axes[0][point[0]], axes[1][point[1]])
            there = (axes[0][x], axes[1][y])
            cells = lane_keys(here, there)
            weight = math.dist(here, there) * (1 + 1.4 * max((used_edges[key] for key in cells), default=0))
            axis = 0 if dx else 1
            lo, hi = sorted((here[axis], there[axis]))
            other = here[1 - axis]
            weight += CROSSING_COST * sum(lo <= value < hi and start_span < other < end_span
                               for _, _, cell in cells
                               for value, start_span, end_span in crossings.get((axis, cell), ()))
            if index != direction:
                weight += 34
            next_cost = cost + weight
            next_state = next_point, index
            if next_cost < costs.get(next_state, float("inf")):
                costs[next_state] = next_cost
                parents[next_state] = state
                remaining = abs(end[0] - there[0]) + abs(end[1] - there[1])
                heapq.heappush(queue, (next_cost + remaining, next_cost, next_point, index))
    raise ValueError(f"No clear connection between {start} and {end}")


def trim_points(points):
    result = []
    for point in points:
        if result and result[-1] == point:
            continue
        if len(result) >= 2:
            first, second = result[-2:]
            if first[0] == second[0] == point[0] or first[1] == second[1] == point[1]:
                result.pop()
        result.append(point)
    return result


def layout(rooms, connections):
    doors = {door["id"]: (room["id"], door) for room in rooms for door in room["doors"]}
    edges = []
    for connection in connections:
        start, end = connection["from"], connection["to"]
        if start not in doors or end not in doors:
            raise ValueError(f"Connection refers to an unknown door: {connection}")
        room_a, door_a = doors[start]
        room_b, door_b = doors[end]
        edges.append((room_a, room_b, SIDE[door_side(door_a)], SIDE[door_side(door_b)]))

    # The Chapter 1 exit faces left, and the Chapter 2 arrival faces right.
    # Put the chapters on those sides so the gate can use their shared margin.
    chapter_two = [room["id"] for room in rooms if room["chapter"] == 2]
    chapter_one = [room["id"] for room in rooms if room["chapter"] == 1]
    positions = place_rooms(chapter_two, [e for e in edges if e[0] in chapter_two and e[1] in chapter_two], "3A", 1)
    one = place_rooms(chapter_one, [e for e in edges if e[0] in chapter_one and e[1] in chapter_one], "1C", -1)
    gap = max(x for x, _ in positions.values()) + CELL_X + GRID * 6 - MARGIN
    gate_offset = one["1C"][1] - positions["3A"][1]
    positions = {room: (x, y + max(0, gate_offset)) for room, (x, y) in positions.items()}
    positions.update({room: (x + gap, y + max(0, -gate_offset)) for room, (x, y) in one.items()})
    positions = align_neighbors(positions, doors, connections)
    width = math.ceil(max(x + CARD_W for x, _ in positions.values()) + MARGIN)
    height = math.ceil(max(y + CARD_H for _, y in positions.values()) + MARGIN)

    ports = {key: door_points(door, positions[room]) for key, (room, door) in doors.items()}
    used_edges = Counter()
    crossings = defaultdict(list)
    paths = [None] * len(edges)
    route_order = sorted(range(len(edges)), key=lambda i: math.dist(
        ports[connections[i]["from"]][1], ports[connections[i]["to"]][1]))
    for i in route_order:
        connection = connections[i]
        start, start_port, start_direction = ports[connection["from"]]
        end, end_port, end_direction = ports[connection["to"]]
        trail = route(start_port, end_port, (width, height), positions, used_edges, crossings, start_direction, end_direction)
        path = trim_points([start, *trail, end])
        used_edges.update(set(key for a, b in zip(path, path[1:]) for key in lane_keys(a, b)))
        for a, b in zip(path, path[1:]):
            axis = 0 if a[1] == b[1] else 1
            value = a[1 - axis]
            crossings[1 - axis, int(value // GRID)].append((value, *sorted((a[axis], b[axis]))))
        paths[i] = path
    return {"width": width, "height": height, "positions": positions, "paths": paths,
            "card": {"width": CARD_W, "height": CARD_H, "imageY": IMAGE_Y, "imageHeight": PLAY_H}}
