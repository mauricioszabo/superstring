const {assert} = require('chai')
const {TextBuffer, MarkerIndex, DisplayLayer} = require('../..')

// These cover the binding itself; the behavior of the display layer is
// specified by text-buffer's display-layer-spec, which drives this class.
describe('DisplayLayer', () => {
  function build ({text, params = {}, folds = []}) {
    const buffer = new TextBuffer(text)
    const foldsLayer = {index: new MarkerIndex()}
    folds.forEach(([start, end], i) => foldsLayer.index.insert(i + 1, start, end))
    const scratch = new Float64Array(4)
    const displayLayer = new DisplayLayer(buffer, foldsLayer, Object.assign({
      tabLength: 4,
      softWrapColumn: Infinity,
      atomicSoftTabs: true,
      foldCharacter: '⋯'
    }, params), scratch)
    return {buffer, foldsLayer, displayLayer, scratch}
  }

  function getScreenLineTexts (displayLayer, startRow, endRow) {
    assert.equal(displayLayer.prepareScreenLines(startRow, endRow) != null, true)
    const [ids, dataIndexes, texts] = displayLayer.buildScreenLines(startRow, endRow, null, new Uint32Array(0), false)
    assert.equal(dataIndexes.length, ids.length)
    return texts
  }

  it('translates positions through soft wraps and folds', () => {
    const {displayLayer, scratch} = build({
      text: 'abc def ghi\njkl\nmno',
      params: {softWrapColumn: 8},
      folds: [[{row: 1, column: 1}, {row: 2, column: 1}]]
    })

    assert.equal(displayLayer.getScreenLineCount(), 3)
    assert.deepEqual(getScreenLineTexts(displayLayer, 0, 3), ['abc def ', 'ghi', 'j⋯no'])

    displayLayer.translateBufferPosition(0, 9, 0)
    assert.deepEqual([scratch[0], scratch[1]], [1, 1])

    displayLayer.translateScreenPosition(2, 2, 0, false)
    assert.deepEqual([scratch[0], scratch[1], scratch[2]], [2, 1, 0])
  })

  it('only sends the contents of screen lines the caller does not already have', () => {
    const {displayLayer} = build({text: 'a\nb\nc'})
    const [ids] = displayLayer.buildScreenLines(0, 3, null, new Uint32Array(0), false)
    const [ids2, dataIndexes] = displayLayer.buildScreenLines(0, 3, null, new Uint32Array([ids[0], 0, ids[2]]), false)
    assert.deepEqual(Array.from(ids2), Array.from(ids))
    assert.deepEqual(Array.from(dataIndexes), [1])
  })

  it('calls JS callbacks for character widths and wrap boundaries', () => {
    const {displayLayer} = build({
      text: 'abcdef',
      params: {
        softWrapColumn: 4,
        ratioForCharacter: (c) => c === 'a' ? 3 : 1,
        isWrapBoundary: () => false
      }
    })
    assert.deepEqual(getScreenLineTexts(displayLayer, 0, displayLayer.getScreenLineCount()), ['ab', 'cdef'])
  })

  it('reports changes after buffer edits', () => {
    const {buffer, displayLayer, scratch} = build({text: 'a\nb\nc'})
    displayLayer.getScreenLineCount()
    displayLayer.bufferWillChange(1)
    buffer.setTextInRange({start: {row: 1, column: 0}, end: {row: 1, column: 1}}, 'x\ny')
    displayLayer.bufferDidChange(1, 0, 1, 1, 2, 1)
    assert.deepEqual([scratch[0], scratch[1], scratch[2]], [1, 1, 2])
    assert.deepEqual(getScreenLineTexts(displayLayer, 0, 4), ['a', 'x', 'y', 'c'])
  })
})
