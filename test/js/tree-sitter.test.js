const fs = require('fs')
const path = require('path')
const {assert} = require('chai')
const {TextBuffer, TreeSitterGrammar, TreeSitterParser} = require('../..')

// These cover the bindings; Pulsar's specs compare the native language mode
// with the web-tree-sitter one.
describe('native Tree-sitter', () => {
  const grammar = new TreeSitterGrammar(fs.readFileSync(path.join(__dirname, 'fixtures', 'tree-sitter-json.wasm')))

  function parse (text) {
    const buffer = new TextBuffer(text)
    const parser = new TreeSitterParser(grammar)
    const tree = parser.parse(buffer, null, null)
    return {buffer, parser, tree, root: tree.rootNode}
  }

  it('loads WASM grammars', () => {
    assert.equal(grammar.name, 'json')
    assert.throws(() => new TreeSitterGrammar(new Uint8Array([0, 1, 2])))
  })

  it('parses straight from a native TextBuffer, with web-tree-sitter-like nodes', () => {
    const {root} = parse('{"a": [1, "é"],\n "b": true}')
    assert.equal(root.type, 'document')
    assert.deepEqual(root.endPosition, {row: 1, column: 11})
    assert.equal(root.tree.rootNode.type, 'document')
    const object = root.firstChild
    assert.equal(object.type, 'object')
    assert.equal(object.childCount, 5)
    const pair = object.firstNamedChild
    assert.equal(pair.text, '"a": [1, "é"]')
    assert.deepEqual(pair.startPosition, {row: 0, column: 1})
    assert.deepEqual(pair.endPosition, {row: 0, column: 14})
    assert.equal(pair.startIndex, 1)
    assert.equal(pair.endIndex, 14)
    assert.equal(pair.parent.type, 'object')
    assert.equal(pair.nextSibling.type, ',')
    assert.equal(pair.childForFieldName('key').text, '"a"')
    assert.deepEqual(root.descendantsOfType('number').map((n) => n.text), ['1'])
    assert.deepEqual(root.descendantsOfType(['string_content'], {row: 1, column: 0}).map((n) => n.text), ['b'])
    assert.equal(root.descendantForIndex(7).type, 'number')
    assert.equal(root.descendantForIndex(8).type, ',')
    assert.isTrue(pair.equals(object.firstNamedChild))
    assert.equal(pair.id, object.firstNamedChild.id)
    assert.isFalse(root.hasError)
    assert.match(root.toString(), /^\(document \(object/)
  })

  it('evaluates predicates like web-tree-sitter and attaches pattern properties', () => {
    const {root} = parse('{"a": 1, "bb": 2, "a": 3}')
    const query = grammar.query(`
      ((string (string_content) @short) (#match? @short "^.$") (#set! kind "short"))
      ((string (string_content) @long) (#not-match? @long "^.$") (#is? test.flag))
      ((pair key: (string (string_content) @k) value: (number) @v) (#eq? @k "a") (#any-of? @v "1" "3"))
    `)
    const captures = query.captures(root, {})
    assert.deepEqual(captures.map((c) => [c.name, c.node.text]), [
      ['short', 'a'], ['k', 'a'], ['v', '1'],
      ['long', 'bb'],
      ['short', 'a'], ['k', 'a'], ['v', '3']
    ])
    assert.deepEqual(captures[0].setProperties, {kind: 'short'})
    assert.isUndefined(captures[0].assertedProperties)
    assert.deepEqual(captures[3].assertedProperties, {'test.flag': null})
    assert.strictEqual(captures[0].setProperties, captures[4].setProperties)

    const ranged = query.captures(root, {startPosition: {row: 0, column: 10}, endPosition: {row: 0, column: 14}})
    assert.deepEqual(ranged.map((c) => c.node.text), ['bb'])
    assert.throws(() => grammar.query('(nope) @x'), /Bad node name 'nope'/)
    assert.throws(() => grammar.query('((string) @s (#eq? @s))'), /Wrong number of arguments to `#eq\?` predicate/)
  })

  it('reparses incrementally after edits, reporting changed ranges', () => {
    const {buffer, parser, tree} = parse('[1, 2, 3]')
    const copy = tree.copy()
    buffer.setTextInRange({start: {row: 0, column: 4}, end: {row: 0, column: 5}}, '"two"')
    tree.edit({
      startIndex: 4, oldEndIndex: 5, newEndIndex: 9,
      startPosition: {row: 0, column: 4}, oldEndPosition: {row: 0, column: 5}, newEndPosition: {row: 0, column: 9}
    })
    const newTree = parser.parse(buffer, tree, null)
    assert.deepEqual(newTree.rootNode.firstChild.namedChildren.map((n) => n.type), ['number', 'string', 'number'])
    const changedRanges = tree.getChangedRanges(newTree)
    assert.deepEqual(changedRanges, [{startIndex: 4, endIndex: 9, startPosition: {row: 0, column: 4}, endPosition: {row: 0, column: 9}}])
    // The copy wasn't edited.
    assert.deepEqual(copy.rootNode.firstChild.namedChildren.map((n) => n.type), ['number', 'number', 'number'])
    tree.delete()
    tree.delete()
    assert.isNull(tree.rootNode)
  })

  it('reads text with CRLF line endings by offset', () => {
    const {root} = parse('{"a":\r\n "x\r"}')
    assert.isFalse(root.hasError)
    const value = root.descendantsOfType('string').pop()
    assert.equal(value.text, '"x\r"')
  })

  it('parses only the included ranges', () => {
    const buffer = new TextBuffer('xx [1] yy [2] zz')
    const parser = new TreeSitterParser(grammar)
    const tree = parser.parse(buffer, null, [{startIndex: 3, endIndex: 6, startPosition: {row: 0, column: 3}, endPosition: {row: 0, column: 6}}])
    const root = tree.rootNode
    assert.isFalse(root.hasError)
    assert.deepEqual(root.descendantsOfType('number').map((n) => n.text), ['1'])
  })
})
