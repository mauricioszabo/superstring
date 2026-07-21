'use strict'

// An implicit treap of immutable string slices. Like VS Code's piece tree, text
// lives in append-only buffers and tree nodes carry subtree length/line-feed
// metadata. This keeps edits and position lookups logarithmic without rebuilding
// the document or its line index.
class Piece {
  constructor (text, start, end, lineStarts) {
    this.text = text
    this.start = start
    this.end = end
    this.lineStarts = lineStarts || scanLines(text)
    this.length = end - start
    this.lines = upperBound(this.lineStarts, end) - upperBound(this.lineStarts, start)
  }

  slice (start, end) { return new Piece(this.text, this.start + start, this.start + end, this.lineStarts) }
}

class Node {
  constructor (piece) {
    this.piece = piece
    this.priority = randomPriority()
    this.left = null
    this.right = null
    update(this)
  }
}

let randomState = 0x9e3779b9
function randomPriority () {
  randomState ^= randomState << 13
  randomState ^= randomState >>> 17
  randomState ^= randomState << 5
  return randomState >>> 0
}

function scanLines (text) {
  const result = []
  for (let i = 0; i < text.length; i++) if (text.charCodeAt(i) === 10) result.push(i)
  return result
}

function upperBound (array, value) {
  let lo = 0
  let hi = array.length
  while (lo < hi) {
    const mid = (lo + hi) >>> 1
    if (array[mid] < value) lo = mid + 1
    else hi = mid
  }
  return lo
}

function length (node) { return node ? node.length : 0 }
function lines (node) { return node ? node.lines : 0 }
function update (node) {
  node.length = length(node.left) + node.piece.length + length(node.right)
  node.lines = lines(node.left) + node.piece.lines + lines(node.right)
  return node
}

function merge (left, right) {
  if (!left) return right
  if (!right) return left
  if (left.priority < right.priority) {
    left.right = merge(left.right, right)
    return update(left)
  }
  right.left = merge(left, right.left)
  return update(right)
}

function split (node, offset) {
  if (!node) return [null, null]
  const leftLength = length(node.left)
  if (offset < leftLength) {
    const parts = split(node.left, offset)
    node.left = parts[1]
    return [parts[0], update(node)]
  }
  if (offset > leftLength + node.piece.length) {
    const parts = split(node.right, offset - leftLength - node.piece.length)
    node.right = parts[0]
    return [update(node), parts[1]]
  }
  const local = offset - leftLength
  const before = local ? new Node(node.piece.slice(0, local)) : null
  const after = local < node.piece.length ? new Node(node.piece.slice(local, node.piece.length)) : null
  return [merge(node.left, before), merge(after, node.right)]
}

class PieceTree {
  constructor (text) { this.reset(text || '') }

  reset (text) {
    this.root = text.length ? new Node(new Piece(text, 0, text.length)) : null
    this.cachedText = text
  }

  get length () { return length(this.root) }
  get lineCount () { return lines(this.root) + 1 }

  replace (start, end, text) {
    const a = split(this.root, start)
    const b = split(a[1], end - start)
    const inserted = text.length ? new Node(new Piece(text, 0, text.length)) : null
    this.root = merge(merge(a[0], inserted), b[1])
    this.cachedText = null
  }

  getText () {
    if (this.cachedText != null) return this.cachedText
    const chunks = []
    this._visit(this.root, piece => chunks.push(piece.text.slice(piece.start, piece.end)))
    return (this.cachedText = chunks.join(''))
  }

  substring (start, end) {
    if (start >= end) return ''
    const chunks = []
    this._range(this.root, start, end, chunks)
    return chunks.join('')
  }

  charCodeAt (offset) {
    if (offset < 0 || offset >= this.length) return NaN
    let node = this.root
    while (node) {
      const leftLength = length(node.left)
      if (offset < leftLength) node = node.left
      else if (offset >= leftLength + node.piece.length) {
        offset -= leftLength + node.piece.length
        node = node.right
      } else return node.piece.text.charCodeAt(node.piece.start + offset - leftLength)
    }
    return NaN
  }

  // Offset immediately after the row-th LF (zero for row zero).
  lineStart (row) {
    if (row <= 0) return 0
    if (row > lines(this.root)) return this.length
    let node = this.root
    let base = 0
    let wanted = row
    while (node) {
      const leftLines = lines(node.left)
      if (wanted <= leftLines) { node = node.left; continue }
      base += length(node.left)
      wanted -= leftLines
      if (wanted <= node.piece.lines) {
        const first = upperBound(node.piece.lineStarts, node.piece.start)
        return base + node.piece.lineStarts[first + wanted - 1] - node.piece.start + 1
      }
      wanted -= node.piece.lines
      base += node.piece.length
      node = node.right
    }
    return this.length
  }

  positionForOffset (offset) {
    offset = Math.max(0, Math.min(this.length, offset))
    let node = this.root
    let row = 0
    let consumed = 0
    while (node) {
      const leftLength = length(node.left)
      if (offset < consumed + leftLength) { node = node.left; continue }
      row += lines(node.left)
      const pieceStart = consumed + leftLength
      if (offset <= pieceStart + node.piece.length) {
        const localEnd = node.piece.start + offset - pieceStart
        row += upperBound(node.piece.lineStarts, localEnd) - upperBound(node.piece.lineStarts, node.piece.start)
        return {row, column: offset - this.lineStart(row)}
      }
      row += node.piece.lines
      consumed = pieceStart + node.piece.length
      node = node.right
    }
    return {row, column: offset - this.lineStart(row)}
  }

  _visit (node, callback) {
    if (!node) return
    this._visit(node.left, callback)
    callback(node.piece)
    this._visit(node.right, callback)
  }

  _range (node, start, end, chunks) {
    if (!node || start >= end) return
    const leftLength = length(node.left)
    if (start < leftLength) this._range(node.left, start, Math.min(end, leftLength), chunks)
    const pieceEnd = leftLength + node.piece.length
    if (end > leftLength && start < pieceEnd) {
      const a = Math.max(0, start - leftLength)
      const b = Math.min(node.piece.length, end - leftLength)
      chunks.push(node.piece.text.slice(node.piece.start + a, node.piece.start + b))
    }
    if (end > pieceEnd) this._range(node.right, Math.max(0, start - pieceEnd), end - pieceEnd, chunks)
  }
}

module.exports = PieceTree
