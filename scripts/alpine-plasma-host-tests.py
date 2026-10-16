#!/usr/bin/env python3
import base64,importlib.util,io,pathlib,unittest
from PIL import Image
s=importlib.util.spec_from_file_location('check',pathlib.Path(__file__).with_name('alpine-plasma-check.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
def evidence(im=None):
 if im is None:
  im=Image.new('RGBA',(800,600));im.putdata([(x%256,y%256,(x+y)%256,255) for y in range(600) for x in range(800)])
 b=io.BytesIO();im.save(b,format='PNG')
 return b'PLASMA_DESKTOPS=desktops=1 panels=1\nPLASMA_WALLPAPER_CONFIGURED=1\nALPINE_PLASMA_PNG_BEGIN\n'+base64.b64encode(b.getvalue())+b'\nALPINE_PLASMA_PNG_END\n'
class Test(unittest.TestCase):
 def test_good_evidence(self):self.assertEqual(m.check(evidence()),1)
 def test_blank_or_panel_only(self):
  im=Image.new('RGBA',(800,600),'black');im.paste('gray',(0,550,800,600))
  with self.assertRaises(ValueError):m.check(evidence(im))
 def test_wrong_size(self):
  with self.assertRaises(ValueError):m.check(evidence(Image.new('RGBA',(128,96),'red')))
 def test_missing_required_markers(self):
  good=evidence()
  for x in [b'PLASMA_DESKTOPS=desktops=1 panels=1',b'PLASMA_WALLPAPER_CONFIGURED=1',b'ALPINE_PLASMA_PNG_BEGIN']:
   with self.subTest(x=x),self.assertRaises(ValueError):m.check(good.replace(x,b''))
 def test_missing_qml(self):
  with self.assertRaises(ValueError):m.check(evidence()+b'error when loading applet "org.kde.plasma.notifications"')
 def test_transparent(self):
  with self.assertRaises(ValueError):m.check(evidence(Image.new('RGBA',(800,600),(100,120,140,0))))
if __name__=='__main__':unittest.main()
