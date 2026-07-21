'use strict'

// Storage adapter that backs TextBuffer with the piece-tree text buffer
// extracted from VS Code (npm: vscode-textbuffer, MIT). All coordinates
// exposed by this class are superstring's: 0-based {row, column} points and
// 0-based character offsets. The piece tree's 1-based lineNumber/column
// never leak past this file, and this is the only file allowed to require
// vscode-textbuffer — swapping the backend means reimplementing this class.
//
// The one semantic mismatch between the two worlds is the lone `\r`: the
// piece tree treats a `\r` not followed by `\n` as a line break, superstring
// does not (lines split on `\n` only). The adapter therefore runs in two
// modes, tracked by an incrementally-maintained count of lone `\r`s:
//
// - fast mode (no lone `\r`, the overwhelmingly common case): piece-tree
//   line breaks coincide exactly with superstring's, so line and position
//   queries map straight onto the tree's O(log n) native operations.
// - degraded mode (at least one lone `\r`): the tree remains the source of
//   truth for the text itself, but line/position queries fall back to a
//   flat line-starts index rebuilt from the full text after each edit —
//   the same cost profile the previous string-based implementation paid
//   for every edit, so this is never a regression.

const {PieceTreeTextBufferBuilder} = require('vscode-textbuffer')
const {Range} = require('vscode-textbuffer/lib/common/range')

// DefaultEndOfLine.LF — a TypeScript const enum, absent from the compiled
// exports, so the numeric value is used directly.
const DEFAULT_EOL_LF = 1

const CR = 13
const LF = 10

// The tree is always built empty and filled with a single insert() rather
// than through acceptChunk(): the v1.0.0 builder strips a leading UTF-8 BOM
// from the content, and its held-back-`\r` machinery duplicates the
// character when the entire input is one `\r` (or lone high surrogate).
// insert() has neither problem and splits oversized text into
// AverageBufferSize chunks itself, with correct handling of CRLF and
// surrogate pairs at chunk boundaries. finish(false): never normalize line
// endings — mixed CRLF/LF/lone-CR content must round-trip byte for byte.
function buildTree (text) {
  const builder = new PieceTreeTextBufferBuilder()
  const tree = builder.finish(false).create(DEFAULT_EOL_LF)
  if (text.length > 0) tree.insert(0, text, false)
  return tree
}

// Count `\r`s in `text` that are lone, i.e. not immediately followed by
// `\n`. `nextCharCode` is the char code right after `text` in the buffer
// (-1 at end of buffer; a `\r` at end of buffer is lone).
function countLoneCR (text, nextCharCode) {
  let count = 0
  let i = text.indexOf('\r')
  while (i !== -1) {
    const next = i + 1 < text.length ? text.charCodeAt(i + 1) : nextCharCode
    if (next !== LF) count++
    i = text.indexOf('\r', i + 1)
  }
  return count
}

class PieceTreeText {
  constructor (text = '') {
    this._assign(text)
  }

  _assign (text) {
    this._tree = buildTree(text)
    this._loneCRCount = countLoneCR(text, -1)
    this._cachedText = text
    this._cachedExtent = null
    this._flatLineStarts = null
  }

  _invalidate () {
    this._cachedText = null
    this._cachedExtent = null
    this._flatLineStarts = null
  }

  setText (text) {
    this._assign(text)
  }

  get length () {
    return this._tree.getLength()
  }

  get lineCount () {
    if (this._loneCRCount === 0) return this._tree.getLineCount()
    return this._getFlatLineStarts().length
  }

  getText () {
    if (this._cachedText === null) {
      this._cachedText = this._tree.getLinesRawContent()
    }
    return this._cachedText
  }

  // Raw text of [startOffset, endOffset). Out-of-range arguments clamp.
  slice (startOffset, endOffset) {
    const length = this._tree.getLength()
    if (startOffset < 0) startOffset = 0
    if (endOffset > length) endOffset = length
    if (endOffset <= startOffset) return ''
    if (this._cachedText !== null) {
      return this._cachedText.slice(startOffset, endOffset)
    }
    const start = this._tree.getPositionAt(startOffset)
    const end = this._tree.getPositionAt(endOffset)
    return this._tree.getValueInRange(
      new Range(start.lineNumber, start.column, end.lineNumber, end.column))
  }

  // Replace [startOffset, endOffset) with newText. Callers must pass
  // clamped offsets (0 <= start <= end <= length). oldText is the text
  // currently in the range — callers always have it for patch bookkeeping,
  // and it spares a redundant tree read here.
  splice (startOffset, endOffset, newText, oldText) {
    if (oldText === undefined) oldText = this.slice(startOffset, endOffset)

    // Only `\r`s whose lookahead character changes can flip between lone
    // and paired: those inside the replaced window, plus the one just
    // before it. The character *after* the window is unchanged by the
    // edit, so it serves as lookahead for both the old and new window.
    const before = startOffset > 0 ? this.charCodeAt(startOffset - 1) : -1
    const after = endOffset < this._tree.getLength() ? this.charCodeAt(endOffset) : -1
    if (before === CR || newText.length > 0 || oldText.length > 0) {
      const beforeStr = before === -1 ? '' : String.fromCharCode(before)
      this._loneCRCount +=
        countLoneCR(beforeStr + newText, after) -
        countLoneCR(beforeStr + oldText, after)
    }

    if (endOffset > startOffset) this._tree.delete(startOffset, endOffset - startOffset)
    if (newText.length > 0) this._tree.insert(startOffset, newText, false)
    this._invalidate()
  }

  charCodeAt (offset) {
    if (offset < 0 || offset >= this._tree.getLength()) return -1
    if (this._cachedText !== null) return this._cachedText.charCodeAt(offset)
    return this.slice(offset, offset + 1).charCodeAt(0)
  }

  charAt (offset) {
    if (offset < 0 || offset >= this._tree.getLength()) return ''
    if (this._cachedText !== null) return this._cachedText[offset]
    return this.slice(offset, offset + 1)
  }

  // ---------------------------------------------------------------------
  // Position <-> offset (superstring semantics, see text-buffer.js history:
  // row/column clamp rules and the "unreachable point" fallthrough)
  // ---------------------------------------------------------------------

  positionForOffset (offset) {
    if (offset <= 0) return {row: 0, column: 0}
    const length = this._tree.getLength()
    if (offset > length) offset = length
    if (this._loneCRCount === 0) {
      const pos = this._tree.getPositionAt(offset)
      return {row: pos.lineNumber - 1, column: pos.column - 1}
    }
    const starts = this._getFlatLineStarts()
    let lo = 0
    let hi = starts.length - 1
    while (lo < hi) {
      const mid = (lo + hi + 1) >>> 1
      if (starts[mid] <= offset) lo = mid
      else hi = mid - 1
    }
    return {row: lo, column: offset - starts[lo]}
  }

  // Walk-the-text semantics: a column beyond the content of a non-last row
  // (content includes a trailing `\r`) is unreachable and falls through to
  // the end of the buffer; the last row clamps.
  offsetForPosition (point) {
    let row = point.row
    let column = point.column
    if (row < 0) { row = 0; column = 0 }
    if (column < 0) column = 0
    const length = this._tree.getLength()
    const lineCount = this.lineCount
    if (row >= lineCount) return length
    const lineStart = this.lineStartOffset(row)
    if (row + 1 < lineCount) {
      const lineLen = this.lineStartOffset(row + 1) - 1 - lineStart
      if (column > lineLen) return length
      return lineStart + column
    }
    const lineLen = length - lineStart
    if (column > lineLen) column = lineLen
    return lineStart + column
  }

  // Offset of the first character of `row`. Callers must pass
  // 0 <= row < lineCount.
  lineStartOffset (row) {
    if (this._loneCRCount === 0) return this._tree.getOffsetAt(row + 1, 1)
    return this._getFlatLineStarts()[row]
  }

  getExtent () {
    if (this._cachedExtent === null) {
      const lastRow = this.lineCount - 1
      this._cachedExtent = {
        row: lastRow,
        column: this._tree.getLength() - this.lineStartOffset(lastRow)
      }
    }
    return this._cachedExtent
  }

  // ---------------------------------------------------------------------
  // Line queries. A line's content excludes its terminator; a trailing `\r`
  // belongs to the terminator only when a `\n` follows (so the last line
  // keeps a trailing `\r`). All return undefined for out-of-range rows.
  // ---------------------------------------------------------------------

  lineForRow (row) {
    const lineCount = this.lineCount
    if (row < 0 || row >= lineCount) return undefined
    if (this._loneCRCount === 0) return this._tree.getLineContent(row + 1)
    return this._flatLineText(row)
  }

  // The tree's own getLineLength is unusable here: at v1.0.0 it subtracts
  // the buffer's *default* EOL length instead of the line's actual
  // terminator, over-counting CRLF lines by one. getLineContent strips the
  // real terminator (and caches the last visited line), so both length and
  // ending are derived from it instead.
  lineLengthForRow (row) {
    const lineCount = this.lineCount
    if (row < 0 || row >= lineCount) return undefined
    if (this._loneCRCount === 0) return this._tree.getLineContent(row + 1).length
    return this._flatLineContentEnd(row) - this._getFlatLineStarts()[row]
  }

  lineEndingForRow (row) {
    const lineCount = this.lineCount
    if (row < 0 || row >= lineCount) return undefined
    if (row === lineCount - 1) return ''
    if (this._loneCRCount === 0) {
      const endingLen = this._tree.getOffsetAt(row + 2, 1) -
        this._tree.getOffsetAt(row + 1, 1) - this._tree.getLineContent(row + 1).length
      return endingLen === 2 ? '\r\n' : '\n'
    }
    const starts = this._getFlatLineStarts()
    const nlOffset = starts[row + 1] - 1
    if (nlOffset > starts[row] && this._flatCharCodeAt(nlOffset - 1) === CR) return '\r\n'
    return '\n'
  }

  // {text, ending} in one call, for TextBuffer's _getLineInfo/_clipPoint.
  lineInfoForRow (row) {
    const text = this.lineForRow(row)
    if (text === undefined) return undefined
    return {text, ending: this.lineEndingForRow(row)}
  }

  getLines () {
    if (this._loneCRCount === 0) return this._tree.getLinesContent()
    const lineCount = this.lineCount
    const lines = new Array(lineCount)
    for (let row = 0; row < lineCount; row++) lines[row] = this._flatLineText(row)
    return lines
  }

  hasLoneCR () {
    return this._loneCRCount > 0
  }

  hasAstral () {
    if (this._cachedText !== null) return containsHighSurrogate(this._cachedText)
    const snapshot = this._tree.createSnapshot('')
    let chunk
    while ((chunk = snapshot.read()) !== null) {
      if (containsHighSurrogate(chunk)) return true
    }
    return false
  }

  // ---------------------------------------------------------------------
  // Degraded-mode flat index (only built when the buffer contains a lone \r)
  // ---------------------------------------------------------------------

  _getFlatLineStarts () {
    if (this._flatLineStarts === null) {
      const text = this.getText()
      const starts = [0]
      let nl = text.indexOf('\n')
      while (nl !== -1) {
        starts.push(nl + 1)
        nl = text.indexOf('\n', nl + 1)
      }
      this._flatLineStarts = starts
    }
    return this._flatLineStarts
  }

  _flatCharCodeAt (offset) {
    return this.getText().charCodeAt(offset)
  }

  // End of a row's content (before the terminator), degraded mode.
  _flatLineContentEnd (row) {
    const text = this.getText()
    const starts = this._getFlatLineStarts()
    const hasNextLine = row + 1 < starts.length
    let end = hasNextLine ? starts[row + 1] - 1 : text.length
    if (hasNextLine && end > starts[row] && text.charCodeAt(end - 1) === CR) end--
    return end
  }

  _flatLineText (row) {
    return this.getText().slice(this._getFlatLineStarts()[row], this._flatLineContentEnd(row))
  }
}

function containsHighSurrogate (text) {
  for (let i = 0; i < text.length; i++) {
    const code = text.charCodeAt(i)
    if (code >= 0xD800 && code <= 0xDBFF) return true
  }
  return false
}

module.exports = {PieceTreeText}
