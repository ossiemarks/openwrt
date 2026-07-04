'use strict';
'require view';
'require poll';

return view.extend({
    render: function() {
        var container = document.createElement('div');
        container.innerHTML = `
            <h2>CSI Monitor</h2>
            <div style="display:flex;gap:24px;flex-wrap:wrap">
                <div style="padding:16px;border:1px solid #ccc;border-radius:8px;min-width:200px">
                    <h3>Presence</h3>
                    <p id="presence-status">Loading...</p>
                </div>
                <div style="padding:16px;border:1px solid #ccc;border-radius:8px;min-width:200px">
                    <h3>Zones</h3>
                    <p id="zones-status">Loading...</p>
                </div>
                <div style="padding:16px;border:1px solid #ccc;border-radius:8px;min-width:200px">
                    <h3>Vitals</h3>
                    <p id="vitals-status">Loading...</p>
                </div>
                <div style="padding:16px;border:1px solid #ccc;border-radius:8px;min-width:200px">
                    <h3>Status</h3>
                    <p id="health-status">Loading...</p>
                </div>
            </div>`;

        poll.add(function() {
            fetch('/api/csi/presence')
                .then(function(r) { return r.json(); })
                .then(function(d) {
                    document.getElementById('presence-status').textContent =
                        d.present
                        ? ('Present — ' + d.count + ' person(s), confidence: ' +
                           (d.confidence * 100).toFixed(0) + '%')
                        : 'Not present';
                }).catch(function() {});

            fetch('/api/csi/zones')
                .then(function(r) { return r.json(); })
                .then(function(d) {
                    var parts = [];
                    for (var k in d) parts.push(k + ': ' + d[k]);
                    document.getElementById('zones-status').textContent =
                        parts.length ? parts.join(', ') : 'No zones';
                }).catch(function() {});

            fetch('/api/csi/vitals')
                .then(function(r) { return r.json(); })
                .then(function(d) {
                    document.getElementById('vitals-status').textContent =
                        d.feasible
                        ? ('Resp: ' + d.respiration_bpm + ' bpm, HR: ' +
                           d.heart_rate_bpm + ' bpm, conf: ' +
                           (d.confidence * 100).toFixed(0) + '%')
                        : ('Unavailable: ' + d.reason);
                }).catch(function() {});

            fetch('/api/csi/status')
                .then(function(r) { return r.json(); })
                .then(function(d) {
                    document.getElementById('health-status').textContent =
                        'Frames: ' + d.frames_total + ', rate: ' +
                        d.frame_rate.toFixed(1) + '/s';
                }).catch(function() {});
        }, 3);

        return container;
    },
    handleSave: null,
    handleSaveApply: null,
    handleReset: null
});
