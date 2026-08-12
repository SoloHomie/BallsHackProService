import os, json, hashlib

os.chdir('/www/wwwroot/BallsHackProService/releases/latest')
files = []
for root, dirs, filenames in os.walk('.'):
    for fn in filenames:
        if fn == 'manifest.json' or fn == 'BallsHackPro-latest.zip':
            continue
        fp = os.path.join(root, fn)
        rel = fp[2:].replace('\\', '/')
        with open(fp, 'rb') as f:
            data = f.read()
        md5 = hashlib.md5(data).hexdigest()
        files.append({
            'path': rel,
            'md5': md5,
            'size': len(data),
            'url': f'/api/dl?file=latest/{rel}'
        })

with open('manifest.json', 'w') as f:
    json.dump(files, f, separators=(',', ':'))
print(f'Done: {len(files)} files')
