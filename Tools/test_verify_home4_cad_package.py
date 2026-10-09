import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SOURCE = Path(__file__).with_name('verify_home4_cad_package.py')
spec = importlib.util.spec_from_file_location('verify_home4_cad_package', SOURCE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ManifestTests(unittest.TestCase):
    def test_regular_internal_inventory_and_changed_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            license = root / 'licenses/dependency-license-texts/kernel/LICENSE'
            license.parent.mkdir(parents=True); license.write_text('Actual supplied license')
            files = {str(license.relative_to(root)): {'bytes': license.stat().st_size, 'sha256': module.sha(license)}}
            for package in ('libglib', 'pcre2'):
                extra = root / ('licenses/dependency-license-texts/' + package + '/LICENSE.txt')
                extra.parent.mkdir(parents=True); extra.write_text('Copyright\n' + 'terms\n' * 200)
                files[str(extra.relative_to(root))] = {'bytes': extra.stat().st_size, 'sha256': module.sha(extra)}
            root.joinpath('runtime-manifest.json').write_text(json.dumps({'files': files}))
            self.assertEqual(module.verify_files(root)['files'], files)
            extra = root / 'unlisted.py'
            extra.write_text('unlisted code')
            with self.assertRaisesRegex(ValueError, 'unlisted'):
                module.verify_files(root)
            extra.unlink()
            license.write_text('Changed bytes')
            with self.assertRaisesRegex(ValueError, 'differs'):
                module.verify_files(root)

    def test_inventory_path_cannot_escape_or_follow_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); outside = root / 'outside'; outside.write_text('x')
            for name in ('../outside', '/tmp/outside'):
                root.joinpath('runtime-manifest.json').write_text(json.dumps({'files': {name: {'bytes': 1, 'sha256': module.sha(outside)}}}))
                with self.assertRaisesRegex(ValueError, 'Unsafe'):
                    module.verify_files(root)
            root.joinpath('link').symlink_to(outside)
            root.joinpath('runtime-manifest.json').write_text(json.dumps({'files': {'link': {'bytes': 1, 'sha256': module.sha(outside)}}}))
            with self.assertRaisesRegex(ValueError, 'regular'):
                module.verify_files(root)

    def test_runtime_path_is_the_shipped_ue_content_path(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'Studio.app'
            runtime = app / 'Contents/UE/LBMStudio/Content/ThirdParty/Home4CAD'
            runtime.mkdir(parents=True); runtime.joinpath('runtime-manifest.json').write_text('{}')
            self.assertEqual(module.runtime_in(app), runtime)
            duplicate = app / 'Contents/Other/Home4CAD'; duplicate.mkdir(parents=True); duplicate.joinpath('runtime-manifest.json').write_text('{}')
            with self.assertRaisesRegex(ValueError, 'exactly one'):
                module.runtime_in(app)


if __name__ == '__main__':
    unittest.main()
