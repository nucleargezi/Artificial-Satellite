// Custom path routing script for VSCode cph-ng (cph-ng.companion.customPathScript)

const process = async () => {
  if (!workspaceFolders || workspaceFolders.length === 0) {
    return 'No workspace folder found';
  }

  let folder = null;
  if (workspaceFolders.length > 1) {
    const selectedFolderName = await ui.chooseItem(
      workspaceFolders.map((f) => f.name),
      'Select a workspace folder to create files in',
    );
    if (selectedFolderName === undefined) {
      return 'No workspace folder selected';
    }
    const selectedFolder = workspaceFolders.find(
      (f) => f.name === selectedFolderName,
    );
    if (!selectedFolder) {
      return 'Selected workspace folder not found';
    }
    folder = selectedFolder.path;
  } else {
    folder = workspaceFolders[0].path;
  }

  const ext = 'cpp';

  const stripCppSuffix = (s) =>
    s.replace(/\.(?:cpp|cc|cxx)+$/i, '').trim();

  const buildBaseFromTitle = (rawTitle) => {
    const cleaned = stripCppSuffix(String(rawTitle || '').trim());
    const words = cleaned.match(/[\p{L}]+|[0-9]+/gu);
    const base = words
      ? words.join('_')
      : cleaned.replace(/\W+/g, '_');
    return base || 'problem';
  };

  const isHost = (u, host) =>
    u.hostname === host || u.hostname.endsWith(`.${host}`);

  const usedFilenames = new Set();
  const results = [];

  for (let idx = 0; idx < problems.length; ++idx) {
    const problem = problems[idx];
    try {
      const { name, url } = problem;
      let stem = null;

      if (url) {
        try {
          const u = new URL(url);
          if (isHost(u, 'codeforces.com')) {
            const regexPatterns = [
              /\/contest\/(\d+)\/problem\/(\w+)/,
              /\/problemset\/problem\/(\d+)\/(\w+)/,
              /\/gym\/(\d+)\/problem\/(\w+)/,
            ];
            for (const regex of regexPatterns) {
              const match = u.pathname.match(regex);
              if (match) {
                stem = `${match[1]}${match[2]}`;
                break;
              }
            }
          }
          if (!stem && isHost(u, 'luogu.com.cn')) {
            const match = u.pathname.match(/\/problem\/([A-Za-z0-9_]+)/);
            if (match) {
              stem = match[1];
            }
          }
          if (!stem && isHost(u, 'qoj.ac')) {
            const match = u.pathname.match(/\/problem\/(\w+)/);
            if (match) {
              stem = `qoj${match[1]}`;
            }
          }
          if (!stem && isHost(u, 'atcoder.jp')) {
            const match = u.pathname.match(/\/tasks\/(\w+)_(\w+)/);
            if (match) {
              stem = `${match[1]}${match[2]}`;
            }
          }
          if (!stem && isHost(u, 'uoj.ac')) {
            const match = u.pathname.match(/\/contest\/(\d+)\/problem\/(\w+)/);
            if (match) {
              stem = `uoj${match[2]}`;
            }
          }
          if (!stem && isHost(u, 'ac.nowcoder.com')) {
            const match = u.pathname.match(/\/acm\/contest\/(\d+)\/([A-Za-z0-9]+)/);
            if (match) {
              stem = `nc${match[1]}${match[2]}`;
            }
          }
        } catch (e) {
          logger.error('URL Parsing error:', e.message);
        }
      }

      if (!stem) {
        stem = buildBaseFromTitle(name);
      }

      stem = utils.sanitize(stem).trim() || 'problem';
      let filename = `${stem}.${ext}`;
      if (usedFilenames.has(filename)) {
        let candidate = `${stem}_${idx + 1}.${ext}`;
        let suffix = 2;
        while (usedFilenames.has(candidate)) {
          candidate = `${stem}_${idx + 1}_${suffix++}.${ext}`;
        }
        filename = candidate;
      }
      usedFilenames.add(filename);
      results.push(path.join(folder, filename));
    } catch (e) {
      logger.error(
        'Error generating filename for problem',
        e && e.message ? e.message : e,
      );
      results.push(null);
    }
  }

  return results;
};
