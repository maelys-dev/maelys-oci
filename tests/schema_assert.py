# SPDX-License-Identifier: MPL-2.0
"""Assert the JSON Schema subset used by OCI reports, without test dependencies.

Unknown assertions fail the test instead of silently weakening validation.
This checks actual documents against the catalog, not a second field list.
"""
import re


def assert_schema(value, schema, path='$'):
    if isinstance(schema, bool):
        assert schema, f'{path}: forbidden value'
        return
    known = {'type', 'const', 'required', 'properties', 'additionalProperties',
             'items', 'minItems', 'maxItems', 'minimum', 'pattern', 'description'}
    assert not schema.keys() - known, f'{path}: unsupported schema keywords {schema.keys() - known}'
    kinds = {'object': dict, 'array': list, 'string': str, 'integer': int, 'boolean': bool}
    if 'type' in schema:
        assert type(value) is kinds[schema['type']], f'{path}: expected {schema["type"]}'
    if 'const' in schema:
        assert type(value) is type(schema['const']) and value == schema['const'], path
    if 'minimum' in schema and type(value) in (int, float):
        assert value >= schema['minimum'], path
    if 'pattern' in schema and isinstance(value, str):
        assert re.search(schema['pattern'], value), path
    if isinstance(value, dict):
        assert set(schema.get('required', ())) <= value.keys(), f'{path}: missing required member'
        properties = schema.get('properties', {})
        additional = schema.get('additionalProperties', True)
        for key, child in value.items():
            assert_schema(child, properties.get(key, additional), f'{path}.{key}')
    if isinstance(value, list):
        assert len(value) >= schema.get('minItems', 0), path
        assert len(value) <= schema.get('maxItems', len(value)), path
        for index, child in enumerate(value):
            assert_schema(child, schema.get('items', True), f'{path}[{index}]')
