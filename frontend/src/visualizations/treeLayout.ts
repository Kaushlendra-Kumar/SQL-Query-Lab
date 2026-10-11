// Tidy top-down tree layout for plan and AST graphs (no external dependency).
// Leaves are placed on consecutive slots; each parent is centred over its
// children. The root sits at the top, so data flows upward in plan graphs.

export interface TreeLike<T> {
  children: T[];
}

export interface Placed<T> {
  node: T;
  x: number;
  y: number;
  depth: number;
  parent: T | null;
}

export function layoutTree<T extends TreeLike<T>>(root: T, nodeWidth: number, nodeHeight: number, gapX = 28, gapY = 56): Placed<T>[] {
  const out: Placed<T>[] = [];
  let nextSlot = 0;
  const place = (n: T, depth: number, parent: T | null): number => {
    let x: number;
    if (n.children.length === 0) {
      x = nextSlot * (nodeWidth + gapX);
      nextSlot += 1;
    } else {
      const xs = n.children.map((c) => place(c, depth + 1, n));
      x = (xs[0] + xs[xs.length - 1]) / 2;
    }
    out.push({ node: n, x, y: depth * (nodeHeight + gapY), depth, parent });
    return x;
  };
  place(root, 0, null);
  return out;
}

export function flatten<T extends TreeLike<T>>(root: T): T[] {
  const out: T[] = [];
  const walk = (n: T) => {
    out.push(n);
    n.children.forEach(walk);
  };
  walk(root);
  return out;
}
