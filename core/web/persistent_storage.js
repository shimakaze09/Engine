// Web builds only (linked with --pre-js): gives the page a save directory
// that outlives it. /persistent is an IndexedDB-backed mount (IDBFS). The
// page's main() waits on a run dependency until what the browser holds for
// this origin has been read in, so the first load of a save sees it; the
// platform layer points its save base here and calls Module.enginePersist
// after every write it commits under the mount, which copies the mount back
// out. Flushes never overlap: one requested while another runs is folded
// into a single follow-up, and Module.enginePersistIdle() is true once none
// is running or owed.
(function () {
  var kRoot = '/persistent';
  var busy = false;
  var again = false;

  function flush() {
    busy = true;
    FS.syncfs(false, function (error) {
      if (error) {
        console.error('[persistent storage] saving to IndexedDB failed: ' +
                      error);
      }
      if (again) {
        again = false;
        flush();
      } else {
        busy = false;
      }
    });
  }

  Module['enginePersist'] = function () {
    if (busy) {
      again = true;
      return;
    }
    flush();
  };
  Module['enginePersistIdle'] = function () { return !busy && !again; };

  Module['preRun'] = Module['preRun'] || [];
  if (typeof Module['preRun'] === 'function') {
    Module['preRun'] = [Module['preRun']];
  }
  Module['preRun'].push(function () {
    if (!FS.analyzePath(kRoot).exists) {
      FS.mkdir(kRoot);
    }
    FS.mount(IDBFS, {}, kRoot);
    addRunDependency('engine-persistent-storage');
    FS.syncfs(true, function (error) {
      if (error) {
        console.error('[persistent storage] reading from IndexedDB failed: ' +
                      error);
      }
      removeRunDependency('engine-persistent-storage');
    });
  });
})();
