"""Create WPA CPU Usage (Sampled) stack/leaf presets from Microsoft's installed catalog."""
import argparse
import copy
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('catalog', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    ns = 'http://tempuri.org/SerializableElement.xsd'
    ET.register_namespace('', ns)
    ET.register_namespace('xsi', 'http://www.w3.org/2001/XMLSchema-instance')
    tag = lambda name: '{' + ns + '}' + name
    original = ET.parse(args.catalog / 'AppLaunch.wpaProfile')
    graph = next(g for g in original.iter(tag('Graph'))
                 if g.get('Guid') == 'b855361e-7be0-4bc8-a754-3e8507715ca5')
    root = ET.Element(tag('WpaProfileContainer'), Version='2')
    content = ET.SubElement(root, tag('Content'), {'{http://www.w3.org/2001/XMLSchema-instance}type': 'WpaProfile2'})
    ET.SubElement(ET.SubElement(content, tag('Sessions')), tag('Session'), Index='0')
    view = ET.SubElement(ET.SubElement(content, tag('Views')), tag('View'),
                         Guid=str(uuid.uuid5(uuid.NAMESPACE_URL, 'openttd-passenger-cpu-sampled')),
                         Title='Passenger CPU sampled', IsVisible='true')
    graphs = ET.SubElement(view, tag('Graphs'))
    for name, keys in [('Stacks', ['Process', 'Thread ID', 'Stack']),
                       ('Exclusive functions', ['Process', 'Thread ID', 'Module', 'Function'])]:
        target = copy.deepcopy(graph)
        target.attrib.pop('HelpText', None)
        target.set('LayoutStyle', 'DataTable')
        preset = target.find(tag('Preset'))
        preset.attrib.clear()
        preset.attrib.update(Name=name, KeyColumnCount=str(len(keys)),
                             GraphColumnCount=str(len(keys)), RightFrozenColumnCount=str(len(keys) + 2),
                             InitialFilterQuery='[Process Name]:=openttd.exe', InitialFilterShouldKeep='true',
                             InitialExpansionQuery='[Series Depth]:<=1024')
        columns = preset.find(tag('Columns'))
        available = {c.get('Name'): copy.deepcopy(c) for c in columns}
        # The untagged Stack is the last duplicate in the catalog.
        columns.clear()
        for colname in keys + ['Weight', 'Count', 'Process Name']:
            column = available[colname]
            column.set('IsVisible', 'false' if colname == 'Process Name' else 'true')
            column.attrib.pop('SortPriority', None)
            column.attrib.pop('SortOrder', None)
            columns.append(column)
        graphs.append(target)
    ET.SubElement(ET.SubElement(view, tag('SessionIndices')), tag('SessionIndex')).text = '0'
    ET.SubElement(content, tag('ModifiedGraphs'))
    ET.indent(root)
    ET.ElementTree(root).write(args.output, encoding='utf-8', xml_declaration=True)


if __name__ == '__main__':
    main()
