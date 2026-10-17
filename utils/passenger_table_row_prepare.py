"""Copied game TU only: compare dense and compressed v11 rows in one image."""
import argparse
import hashlib
import json
import pathlib
import re


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('source', type=pathlib.Path)
    parser.add_argument('build', type=pathlib.Path)
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    original = source / 'src/passenger_precomputed.cpp'
    cpp = original.read_text(encoding='utf-8')
    before = '''bool PassengerPrecomputedTable::UseCompressedRows() const
{
	return this->integrated;
}'''
    after = '''bool PassengerPrecomputedTable::UseCompressedRows() const
{
	if (!this->integrated) return false;
	static const bool dense = [] {
		bool value = getenv("OPENTTD_PRECOMPUTED_DENSE_ROWS") != nullptr;
		std::fprintf(stderr, "Precomputed row storage control: dense=%d\\n", value ? 1 : 0);
		return value;
	}();
	return !dense;
}'''
    assert cpp.count(before) == 1
    cpp = cpp.replace(before, after)
    assert cpp.count('#include "stdafx.h"') == 1
    cpp = cpp.replace('#include "stdafx.h"', '#include "stdafx.h"\n#include <cstdio>')
    cpp = re.sub(r'#include "([^"]+)"', lambda m: '#include "' + (original.parent / m[1]).resolve().as_posix() + '"' if (original.parent / m[1]).is_file() else m[0], cpp)
    target = build / 'row-memory-driver/passenger_precomputed.cpp'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(cpp, encoding='utf-8')
    project = build / 'openttd_lib.vcxproj'
    xml = project.read_text(encoding='utf-8-sig')
    assert xml.count(str(original)) == 1
    project.write_text(xml.replace(str(original), str(target)), encoding='utf-8-sig')
    metadata = dict(product_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
        copied_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
        note='Only the out-of-line derived Row predicate changes; identical class/header ABI and saved v11 flags. Dense mode has the new empty Row container headers and shared slot map overhead. No product environment switch.')
    (build / 'row-memory-instrumentation.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
