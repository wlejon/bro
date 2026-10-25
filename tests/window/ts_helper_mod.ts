// Helper TypeScript module imported by test_native_typescript.ts

export interface Vec2 {
    x: number;
    y: number;
}

export type TransformFn = (v: Vec2) => Vec2;

export enum Mode {
    Add = 10,
    Scale = 20,
}

export function scaleVec<T extends Vec2>(v: T, factor: number): Vec2 {
    return { x: v.x * factor, y: v.y * factor };
}
