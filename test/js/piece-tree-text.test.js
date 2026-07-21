const {assert} = require('chai')
const Random = require('random-seed')
const {PieceTreeText} = require('../../src/js/piece-tree-text')

// Plain-string reference model implementing the exact semantics the adapter
// must reproduce (lifted from the previous string-based TextBuffer storage:
// lines split on \n only; a trailing \r belongs to the line ending only when
// a \n follows; "unreachable" points fall through to the buffer end).
class ReferenceText {
  constructor (text = '') { this.text = text }

  setText (text) { this.text = text }

  get length () { return this.text.length }

  get lineStarts () {
    const starts = [0]
    for (let i = 0; i < this.text.length; i++) {
      if (this.text.charCodeAt(i) === 10) starts.push(i + 1)
    }
    return starts
  }

  get lineCount () { return this.lineStarts.length }

  getText () { return this.text }

  slice (start, end) { return this.text.slice(Math.max(0, start), end) }

  splice (start, end, newText) {
    this.text = this.text.slice(0, start) + newText + this.text.slice(end)
  }

  charCodeAt (offset) {
    if (offset < 0 || offset >= this.text.length) return -1
    return this.text.charCodeAt(offset)
  }

  charAt (offset) {
    if (offset < 0 || offset >= this.text.length) return ''
    return this.text[offset]
  }

  positionForOffset (offset) {
    if (offset <= 0) return {row: 0, column: 0}
    if (offset > this.text.length) offset = this.text.length
    const starts = this.lineStarts
    let row = 0
    for (let i = 1; i < starts.length && starts[i] <= offset; i++) row = i
    return {row, column: offset - starts[row]}
  }

  offsetForPosition (point) {
    let {row, column} = point
    if (row < 0) { row = 0; column = 0 }
    if (column < 0) column = 0
    const starts = this.lineStarts
    if (row >= starts.length) return this.text.length
    const lineStart = starts[row]
    if (row + 1 < starts.length) {
      const lineLen = starts[row + 1] - 1 - lineStart
      if (column > lineLen) return this.text.length
      return lineStart + column
    }
    const lineLen = this.text.length - lineStart
    if (column > lineLen) column = lineLen
    return lineStart + column
  }

  lineStartOffset (row) { return this.lineStarts[row] }

  _contentEnd (row) {
    const starts = this.lineStarts
    const hasNextLine = row + 1 < starts.length
    let end = hasNextLine ? starts[row + 1] - 1 : this.text.length
    if (hasNextLine && end > starts[row] && this.text.charCodeAt(end - 1) === 13) end--
    return end
  }

  lineForRow (row) {
    if (row < 0 || row >= this.lineCount) return undefined
    return this.text.slice(this.lineStarts[row], this._contentEnd(row))
  }

  lineLengthForRow (row) {
    if (row < 0 || row >= this.lineCount) return undefined
    return this._contentEnd(row) - this.lineStarts[row]
  }

  lineEndingForRow (row) {
    if (row < 0 || row >= this.lineCount) return undefined
    if (row === this.lineCount - 1) return ''
    const nlOffset = this.lineStarts[row + 1] - 1
    if (nlOffset > this.lineStarts[row] && this.text.charCodeAt(nlOffset - 1) === 13) return '\r\n'
    return '\n'
  }

  getLines () {
    const lines = []
    for (let row = 0; row < this.lineCount; row++) lines.push(this.lineForRow(row))
    return lines
  }

  getExtent () {
    const starts = this.lineStarts
    const lastRow = starts.length - 1
    return {row: lastRow, column: this.text.length - starts[lastRow]}
  }

  hasLoneCR () {
    for (let i = 0; i < this.text.length; i++) {
      if (this.text.charCodeAt(i) === 13 && this.text.charCodeAt(i + 1) !== 10) return true
    }
    return false
  }

  hasAstral () {
    for (let i = 0; i < this.text.length; i++) {
      const code = this.text.charCodeAt(i)
      if (code >= 0xD800 && code <= 0xDBFF) return true
    }
    return false
  }
}

function assertMatchesReference (actual, expected, context) {
  assert.strictEqual(actual.getText(), expected.getText(), `${context}: getText`)
  assert.strictEqual(actual.length, expected.length, `${context}: length`)
  assert.strictEqual(actual.lineCount, expected.lineCount, `${context}: lineCount`)
  assert.deepEqual(actual.getExtent(), expected.getExtent(), `${context}: getExtent`)
  assert.strictEqual(actual.hasLoneCR(), expected.hasLoneCR(), `${context}: hasLoneCR`)
  assert.strictEqual(actual.hasAstral(), expected.hasAstral(), `${context}: hasAstral`)
  assert.deepEqual(actual.getLines(), expected.getLines(), `${context}: getLines`)

  for (let offset = -1; offset <= expected.length + 2; offset++) {
    assert.deepEqual(actual.positionForOffset(offset), expected.positionForOffset(offset),
      `${context}: positionForOffset(${offset})`)
    assert.strictEqual(actual.charCodeAt(offset) || -1, expected.charCodeAt(offset) || -1,
      `${context}: charCodeAt(${offset})`)
    assert.strictEqual(actual.charAt(offset), expected.charAt(offset),
      `${context}: charAt(${offset})`)
  }

  for (let row = -1; row <= expected.lineCount + 1; row++) {
    assert.strictEqual(actual.lineForRow(row), expected.lineForRow(row),
      `${context}: lineForRow(${row})`)
    assert.strictEqual(actual.lineLengthForRow(row), expected.lineLengthForRow(row),
      `${context}: lineLengthForRow(${row})`)
    assert.strictEqual(actual.lineEndingForRow(row), expected.lineEndingForRow(row),
      `${context}: lineEndingForRow(${row})`)
    if (row >= 0 && row < expected.lineCount) {
      assert.strictEqual(actual.lineStartOffset(row), expected.lineStartOffset(row),
        `${context}: lineStartOffset(${row})`)
      const maxColumn = (expected.lineLengthForRow(row) || 0) + 3
      for (let column = -1; column <= maxColumn; column++) {
        assert.strictEqual(
          actual.offsetForPosition({row, column}),
          expected.offsetForPosition({row, column}),
          `${context}: offsetForPosition({${row}, ${column}})`)
      }
    }
  }
  assert.strictEqual(actual.offsetForPosition({row: expected.lineCount + 5, column: 0}),
    expected.offsetForPosition({row: expected.lineCount + 5, column: 0}),
    `${context}: offsetForPosition past last row`)
  assert.strictEqual(actual.offsetForPosition({row: -2, column: 7}),
    expected.offsetForPosition({row: -2, column: 7}),
    `${context}: offsetForPosition negative row`)
  assert.strictEqual(actual.offsetForPosition({row: 0, column: Infinity}),
    expected.offsetForPosition({row: 0, column: Infinity}),
    `${context}: offsetForPosition Infinity column`)

  for (let start = 0; start <= expected.length; start += Math.max(1, expected.length >> 3)) {
    for (let end = start; end <= expected.length; end += Math.max(1, expected.length >> 3)) {
      assert.strictEqual(actual.slice(start, end), expected.slice(start, end),
        `${context}: slice(${start}, ${end})`)
    }
  }
}

describe('PieceTreeText', () => {
  const fixtures = [
    '',
    'a',
    '\n',
    '\r',
    '\r\n',
    'abc\r',
    'abc\rdef',
    'abc\r\ndefg\n\r\nhijkl\n\n',
    'no trailing newline',
    'one\ntwo\nthree\n',
    'crlf\r\nlines\r\nonly\r\n',
    'mixed\nendings\r\nhere\rlone',
    '\n\n\n',
    '\r\r\r',
    'ab\u{1F600}cd\n\u{1F600}\r\n\u{1F600}'
  ]

  for (const fixture of fixtures) {
    it(`matches the reference model for ${JSON.stringify(fixture)}`, () => {
      assertMatchesReference(new PieceTreeText(fixture), new ReferenceText(fixture), 'initial')
    })
  }

  it('applies edits and flips between fast and degraded modes', () => {
    const actual = new PieceTreeText('abc\ndef\nghi')
    const expected = new ReferenceText('abc\ndef\nghi')
    const edits = [
      [4, 4, 'X\r'],      // introduces a lone \r (degraded mode)
      [5, 6, ''],         // removes it (back to fast mode)
      [3, 4, '\r\n'],     // replace \n with \r\n
      [0, 0, '\r'],       // lone \r at start
      [0, 1, ''],         // remove it
      [11, 11, '\r'],     // lone \r at end of buffer
      [11, 12, '\r\n'],   // now paired
      [4, 9, ''],         // delete across the CRLF
      [0, Infinity, '']   // clear
    ]
    for (const [start, end, newText] of edits) {
      const clampedEnd = Math.min(end, expected.length)
      const clampedStart = Math.min(start, clampedEnd)
      actual.splice(clampedStart, clampedEnd, newText, actual.slice(clampedStart, clampedEnd))
      expected.splice(clampedStart, clampedEnd, newText)
      assertMatchesReference(actual, expected, `after splice(${clampedStart}, ${clampedEnd}, ${JSON.stringify(newText)})`)
    }
  })

  it('setText resets content and mode', () => {
    const actual = new PieceTreeText('with\rlone\rcrs')
    const expected = new ReferenceText('with\rlone\rcrs')
    assertMatchesReference(actual, expected, 'before setText')
    actual.setText('clean\nagain\n')
    expected.setText('clean\nagain\n')
    assertMatchesReference(actual, expected, 'after setText')
  })

  it('splices at an offset between \\r and \\n', () => {
    const actual = new PieceTreeText('abc\r\ndef')
    const expected = new ReferenceText('abc\r\ndef')
    actual.splice(4, 4, 'X', '')
    expected.splice(4, 4, 'X')
    assertMatchesReference(actual, expected, 'insert mid-CRLF')
    actual.splice(3, 5, '', actual.slice(3, 5))
    expected.splice(3, 5, '')
    assertMatchesReference(actual, expected, 'delete spanning CRLF halves')
  })

  it('handles documents larger than one 64KB chunk', () => {
    const line = 'the quick brown fox jumps over the lazy dog\n'
    const text = line.repeat(3000) // ~132KB
    const actual = new PieceTreeText(text)
    const expected = new ReferenceText(text)
    assert.strictEqual(actual.getText(), text)
    assert.strictEqual(actual.lineCount, 3001)
    assert.deepEqual(actual.getExtent(), expected.getExtent())
    assert.deepEqual(actual.positionForOffset(text.length - 1), expected.positionForOffset(text.length - 1))
    actual.splice(70000, 70010, 'REPLACED', actual.slice(70000, 70010))
    expected.splice(70000, 70010, 'REPLACED')
    assert.strictEqual(actual.getText(), expected.getText())
    assert.deepEqual(actual.positionForOffset(70004), expected.positionForOffset(70004))
  })

  for (let seed = 1; seed <= 6; seed++) {
    it(`differential fuzz against the reference model (seed ${seed})`, function () {
      this.timeout(60000)
      const random = Random(seed)
      const pieces = ['a', 'b', 'xyz', ' ', '\n', '\r', '\r\n', '\n\r', 'q\r', '\nw', '\u{1F600}', 'line one\nline two\r\n']
      const actual = new PieceTreeText('')
      const expected = new ReferenceText('')
      for (let i = 0; i < 200; i++) {
        let newText = ''
        const pieceCount = random(4)
        for (let j = 0; j < pieceCount; j++) newText += pieces[random(pieces.length)]
        const start = random(expected.length + 1)
        const end = start + random(Math.min(10, expected.length - start) + 1)
        const oldText = actual.slice(start, end)
        actual.splice(start, end, newText, oldText)
        expected.splice(start, end, newText)
        assert.strictEqual(actual.getText(), expected.getText(),
          `edit ${i}: splice(${start}, ${end}, ${JSON.stringify(newText)})`)
        if (i % 10 === 0 || expected.length < 40) {
          assertMatchesReference(actual, expected, `edit ${i}`)
        }
      }
      assertMatchesReference(actual, expected, 'final state')
    })
  }
})
