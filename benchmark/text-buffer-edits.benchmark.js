// Editing-workload benchmark for the TextBuffer storage layer.
//
// Run with:  node benchmark/text-buffer-edits.benchmark.js [sizeMB]
//
// To compare implementations, run it once on the commit before the
// piece-tree storage landed and once after; the scenarios and seed are
// deterministic so the numbers are directly comparable.

const Random = require('random-seed')
const {TextBuffer} = require('..')

const sizeMB = Number(process.argv[2]) || 16
const LINE = 'abc def ghi jkl mno pqr stu vwx yz0 123\n'
const LINE_COUNT = Math.floor((sizeMB * 1024 * 1024) / LINE.length)
const TEXT = LINE.repeat(LINE_COUNT)

function time (label, fn) {
  const start = process.hrtime.bigint()
  const result = fn()
  const ms = Number(process.hrtime.bigint() - start) / 1e6
  console.log(`${label}: ${ms.toFixed(1)}ms`)
  return result
}

console.log(`document: ${(TEXT.length / 1024 / 1024).toFixed(1)}MB, ${LINE_COUNT} lines\n`)

{
  const random = Random(42)
  const buffer = time('construct', () => new TextBuffer(TEXT))

  time('A: 10,000 random single-word edits', () => {
    for (let i = 0; i < 10000; i++) {
      const row = random(buffer.getLineCount() - 1)
      const column = random(30)
      buffer.setTextInRange(
        {start: {row, column}, end: {row, column: column + 3}},
        'zzz')
    }
  })

  time('B: 10,000 sequential 1-char typing inserts', () => {
    let column = 0
    const row = Math.floor(buffer.getLineCount() / 2)
    for (let i = 0; i < 10000; i++) {
      buffer.setTextInRange(
        {start: {row, column}, end: {row, column}},
        'x')
      column++
    }
  })

  time('C: 100,000 position<->offset conversions with interleaved edits', () => {
    let acc = 0
    for (let i = 0; i < 1000; i++) {
      const row = random(buffer.getLineCount() - 1)
      buffer.setTextInRange(
        {start: {row, column: 0}, end: {row, column: 1}}, 'y')
      for (let j = 0; j < 100; j++) {
        const offset = random(buffer.getLength())
        const pos = buffer.positionForCharacterIndex(offset)
        acc += buffer.characterIndexForPosition(pos)
      }
    }
    return acc
  })

  time('D: sequential lineForRow sweep', () => {
    let acc = 0
    const lineCount = buffer.getLineCount()
    for (let row = 0; row < lineCount; row++) {
      acc += buffer.lineForRow(row).length
    }
    return acc
  })

  time('E1: findAllSync after an edit', () => {
    buffer.setTextInRange({start: {row: 0, column: 0}, end: {row: 0, column: 1}}, 'a')
    return buffer.findAllSync(/jkl/).length
  })

  time('E2: findAllSync again without edits (cached)', () => {
    return buffer.findAllSync(/jkl/).length
  })

  time('getText after edits', () => buffer.getText().length)
}
