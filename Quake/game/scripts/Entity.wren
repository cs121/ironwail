// Entity.wren
// Lightweight container that mimics the mutable field access pattern used by
// QuakeC entities. The implementation stores properties in a map so that new
// fields can be introduced dynamically as gameplay code touches them.

class GameEntity {
  construct new() {
    _fields = {}
  }

  get(field, defaultValue) {
    if (!_fields.containsKey(field)) return defaultValue
    return _fields[field]
  }

  set(field, value) {
    _fields[field] = value
    return value
  }

  ensure(field, defaultValue) {
    if (!_fields.containsKey(field)) {
      _fields[field] = defaultValue
    }
    return _fields[field]
  }

  increment(field, amount) {
    var current = get(field, 0)
    set(field, current + amount)
    return current + amount
  }

  fields { get { return _fields } }
}
