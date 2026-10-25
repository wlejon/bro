// Native TypeScript (.ts) end-to-end execution test (zero tsc step).
// Exercises interface, type, enum, generic functions, typed classes with
// constructor parameter properties, `as`, `!`, Float32Array annotations,
// and .ts module resolution.

import { type Vec2, type TransformFn, Mode, scaleVec } from './ts_helper_mod.js';

declare function assert(cond: boolean, msg?: string): void;

interface Particle {
    id: string;
    pos: Vec2;
    vel?: Vec2;
}

type ScoreMap = Record<string, number>;

class SimBody {
    public history: number[] = [];

    constructor(
        public readonly label: string,
        private mass: number = 2.5,
        protected active: boolean = true
    ) {}

    public step(buf: Float32Array, dt: number): number {
        if (!this.active) return 0;
        let acc: number = 0;
        for (let i: number = 0; i < buf.length; i++) {
            buf[i] = buf[i] * dt + this.mass;
            acc += buf[i];
        }
        this.history.push(acc);
        return acc;
    }
}

function pickFirst<T>(items: T[]): T {
    return items[0]!;
}

const p: Particle = {
    id: 'p0',
    pos: { x: 3, y: 4 },
};

const scaled: Vec2 = scaleVec(p.pos, 2);
assert(scaled.x === 6 && scaled.y === 8, 'generic scaleVec from .ts module works');
assert(Mode.Add === 10 && Mode.Scale === 20, 'TS enum values work');

const tx: TransformFn = (v: Vec2): Vec2 => ({ x: v.x + 1, y: v.y - 1 });
const moved: Vec2 = tx(scaled);
assert(moved.x === 7 && moved.y === 7, 'typed arrow function works');

const body = new SimBody('alpha', 1.5, true);
assert(body.label === 'alpha', 'constructor parameter property works');

const arr: Float32Array = new Float32Array([2.0, 4.0, 6.0, 8.0]);
const sum: number = body.step(arr, 2.0);
// Each element: x * 2.0 + 1.5 -> [5.5, 9.5, 13.5, 17.5], sum = 46.0
assert(Math.abs(sum - 46.0) < 1e-5, 'Float32Array typed method execution works: ' + sum);
assert(body.history.length === 1 && Math.abs(body.history[0]! - 46.0) < 1e-5,
       'non-null assertion and field array work');

const scores: ScoreMap = { alpha: sum as number };
const topKey: string = pickFirst<string>(Object.keys(scores));
assert(topKey === 'alpha' && scores[topKey]! === 46.0, 'generic function and type assertion work');
