#!/usr/bin/env python3
"""Small negative controls for the fast lane's artifact/lock trust boundary."""
import hashlib, importlib.util, io, pathlib, tempfile, unittest
from unittest.mock import patch
p = pathlib.Path(__file__).resolve().parents[1] / 'userland/alpine/build.py'
spec = importlib.util.spec_from_file_location('alpine_builder', p)
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class TrustTests(unittest.TestCase):
    def test_account_overlay_preserves_distro_groups_and_locked_accounts(self):
        original='root:x:0:\naudio:x:29:\nvideo:x:27:\n'
        overlay='root:x:0:root\nvideo:x:44:root,user\nuser:x:1000:\n'
        out=m.merge_named_records(original,overlay)
        self.assertEqual(out,'root:x:0:root\naudio:x:29:\nvideo:x:44:root,user\nuser:x:1000:\n')
        self.assertEqual(m.merge_named_records(out,overlay),out)
        self.assertIn('daemon:!:0:0:::::',m.merge_named_records('root:*:0:0:::::\ndaemon:!:0:0:::::\n','root::0:0:::::\n'))
    def test_desktop_daemon_identity_is_locked_unprivileged_and_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            stage=pathlib.Path(d);(stage/'etc').mkdir()
            for name,text in [('passwd','root:x:0:0:root:/root:/bin/sh\n'),('group','root:x:0:\n'),('shadow','root::0:0:::::\n')]:
                (stage/'etc'/name).write_text(text)
            m.install_desktop_accounts(stage)
            before={name:(stage/'etc'/name).read_text() for name in ['passwd','group','shadow']}
            self.assertIn('messagebus:x:81:81:',before['passwd'])
            self.assertIn('messagebus:!:',before['shadow'])
            m.install_desktop_accounts(stage)
            self.assertEqual(before,{name:(stage/'etc'/name).read_text() for name in before})
            (stage/'etc/passwd').write_text('other:x:81:81::/:/bin/false\n')
            with self.assertRaises(ValueError):m.install_desktop_accounts(stage)
            (stage/'etc/passwd').write_text('messagebus:x:0:0::/:/bin/false\n')
            with self.assertRaises(ValueError):m.install_desktop_accounts(stage)
    def test_existing_daemon_identity_cannot_keep_root_group_login_or_password(self):
        bad = [
            ('passwd', 'messagebus:x:81:0::/dev/null:/sbin/nologin\n'),
            ('passwd', 'messagebus:x:81:81::/dev/null:/bin/sh\n'),
            ('passwd', 'messagebus:x:82:81::/dev/null:/sbin/nologin\n'),
            ('passwd', 'messagebus:x:81:81::/dev/null:/sbin/nologin\nother:x:81:81::/:/bin/false\n'),
            ('group', 'messagebus:x:0:\n'),
            ('group', 'messagebus:x:81:\nmessagebus:x:81:\n'),
            ('shadow', 'messagebus::0:0:99999:7:::\n'),
            ('shadow', 'messagebus:$6$not-a-locked-password:0:0:99999:7:::\n'),
        ]
        for name, text in bad:
            with self.subTest(name=name, record=text), tempfile.TemporaryDirectory() as d:
                stage=pathlib.Path(d);(stage/'etc').mkdir()
                for key, data in [('passwd','root:x:0:0:root:/root:/bin/sh\n'),('group','root:x:0:\n'),('shadow','root::0:0:::::\n')]:
                    (stage/'etc'/key).write_text(data)
                m.install_desktop_accounts(stage)
                (stage/'etc'/name).write_text(text)
                with self.assertRaises(ValueError):m.install_desktop_accounts(stage)
    def entry(self, **kw):
        return dict(url=m.BASE+'/v3.22/main/x86_64/musl-1.2.5-r12.apk', sha256='a'*64, **kw)
    def test_strict_locked_urls(self):
        good = self.entry(); self.assertEqual(m.validate_entries([good], 'x86_64', True), [good])
        for url in [good['url'].replace('/main/', '/edge/'), good['url'].replace('/x86_64/', '/aarch64/'),
                    good['url'].replace('/musl-', '/../musl-'), good['url']+'?other.apk',
                    good['url'].replace('https:', 'http:')]:
            with self.subTest(url=url), self.assertRaises(ValueError):
                m.validate_entries([dict(good, url=url)], 'x86_64', True)
    def test_required_unique_hashes(self):
        good=self.entry()
        for sha in [None, '', 'g'*64, 'a'*63]:
            with self.subTest(sha=sha), self.assertRaises(ValueError):
                m.validate_entries([dict(good, sha256=sha)], 'x86_64', True)
        with self.assertRaises(ValueError):m.validate_entries([good, good], 'x86_64', True)
        with self.assertRaises(ValueError):m.validate_entries([], 'x86_64', True)
    def test_profile_requires_wayland_packages(self):
        entries=[dict(url=m.BASE+'/v3.22/community/x86_64/'+name+'-1.0-r0.apk') for name in m.PACKAGES]
        entries[0]['url']=m.BASE+'/v3.22/community/x86_64/qt6-qtdeclarative-6.8.3-r0.apk'
        m.validate_profile(entries)
        for i in range(len(entries)):
            with self.subTest(missing=i),self.assertRaises(ValueError):m.validate_profile(entries[:i]+entries[i+1:])
        with self.assertRaises(ValueError):m.validate_profile([dict(e,url=e['url'].replace('6.8.3','6.9.0')) for e in entries])
    def test_desktop_profile_requires_desktop_packages(self):
        entries=[dict(url=m.BASE+'/v3.22/community/x86_64/'+name+'-1.0-r0.apk') for name in m.PROFILES['desktop']]
        entries[0]['url']=m.BASE+'/v3.22/community/x86_64/qt6-qtdeclarative-6.8.3-r0.apk'
        m.validate_profile(entries,'desktop')
        m.validate_profile(entries,'base')
        for missing in ['kwin','plasma-workspace','plasma-desktop','konsole','dolphin','seatd','strace']:
            subset=[e for e in entries if '/'+missing+'-1.0-r0.apk' not in e['url']]
            with self.subTest(missing=missing),self.assertRaises(ValueError):m.validate_profile(subset,'desktop')
    def test_valid_offline_cache_never_uses_network(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d)/'p.apk';p.write_bytes(b'valid')
            with patch.object(m.urllib.request, 'urlopen', side_effect=AssertionError('network')) as network:
                m.download('https://unused', p, hashlib.sha256(b'valid').hexdigest(), offline=True)
                network.assert_not_called()
    def test_corrupt_offline_cache_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d)/'p.apk';p.write_bytes(b'corrupt')
            with patch.object(m.urllib.request, 'urlopen', side_effect=AssertionError('network')) as network:
                with self.assertRaises(ValueError):m.download('https://unused', p, '0'*64, offline=True)
                self.assertEqual(p.read_bytes(), b'corrupt');network.assert_not_called()
    def test_bad_download_is_not_published(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d)/'p.apk'
            with patch.object(m.urllib.request, 'urlopen', side_effect=lambda *a,**kw:io.BytesIO(b'wrong')), patch.object(m.time,'sleep'):
                with self.assertRaises(ValueError):m.download('https://unused',p,'0'*64)
            self.assertFalse(p.exists());self.assertFalse(p.with_suffix('.apk.part').exists())

if __name__ == '__main__':unittest.main()
