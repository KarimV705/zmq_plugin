import xml.etree.ElementTree as ET
import os

def fix_svg(file_path, is_vaporwave=False):
    print(f"Processing {file_path}...")
    ET.register_namespace('', "http://www.w3.org/2000/svg")
    ET.register_namespace('xlink', "http://www.w3.org/1999/xlink")
    ET.register_namespace('serif', "http://www.serif.com/")
    
    tree = ET.parse(file_path)
    root = tree.getroot()
    
    # Force the correct viewBox and dimensions for 10HP module
    root.set('viewBox', '0 0 150 380')
    root.set('width', '100%')
    root.set('height', '100%')
    
    # Identify defs and other top-level elements
    defs_el = None
    other_elements = []
    
    for child in list(root):
        if child.tag.endswith('defs'):
            defs_el = child
        else:
            other_elements.append(child)
            root.remove(child)
            
    # Wrap all graphical elements in a scale(0.24) group to scale 625x1584 down to 150x380
    scaled_group = ET.Element('{http://www.w3.org/2000/svg}g')
    scaled_group.set('transform', 'scale(0.24)')
    for el in other_elements:
        scaled_group.append(el)
        
    root.append(scaled_group)
    
    # If vaporwave, replace complex gradient transforms with simple vertical gradients
    if is_vaporwave and defs_el is not None:
        for gradient in defs_el:
            grad_id = gradient.get('id')
            # Clear gradientTransform
            if 'gradientTransform' in gradient.attrib:
                del gradient.attrib['gradientTransform']
            
            # Set simple vertical gradient coords (pre-scale coords)
            gradient.set('x1', '0')
            gradient.set('x2', '0')
            gradient.set('gradientUnits', 'userSpaceOnUse')
            
            if grad_id == '_Linear1':
                gradient.set('y1', '0')
                gradient.set('y2', '1584')
            elif grad_id == '_Linear3':
                gradient.set('y1', '559')
                gradient.set('y2', '1584')
            elif grad_id == '_Linear4':
                gradient.set('y1', '1114')
                gradient.set('y2', '1584')
                
    # Save back to file
    tree.write(file_path, encoding='utf-8', xml_declaration=True)
    print(f"Successfully fixed {file_path}")

if __name__ == '__main__':
    fix_svg('res/ZeroMQSocket_jungle.svg', is_vaporwave=False)
    fix_svg('res/ZeroMQSocket_vaporwave.svg', is_vaporwave=True)
