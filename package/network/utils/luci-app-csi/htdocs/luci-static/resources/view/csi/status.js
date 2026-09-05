'use strict';
'require view';
'require rpc';
'require poll';

var callStatus = rpc.declare({
    object: 'csi',
    method: 'status',
    expect: {}
});

var callPresence = rpc.declare({
    object: 'csi',
    method: 'presence',
    expect: {}
});

var callVitals = rpc.declare({
    object: 'csi',
    method: 'vitals',
    expect: {}
});

function card(title, id) {
    return E('div', {
        'style': 'padding:16px;border:1px solid #ccc;border-radius:8px;min-width:200px'
    }, [
        E('h3', {}, title),
        E('p', { 'id': id }, 'Loading...')
    ]);
}

return view.extend({
    render: function() {
        var container = E('div', {}, [
            E('h2', {}, 'CSI Monitor'),
            E('div', {
                'style': 'display:flex;gap:24px;flex-wrap:wrap'
            }, [
                card('Presence', 'presence-status'),
                card('Vitals', 'vitals-status'),
                card('Status', 'health-status')
            ])
        ]);

        poll.add(function() {
            return Promise.all([
                callPresence().catch(function() { return null; }),
                callVitals().catch(function() { return null; }),
                callStatus().catch(function() { return null; })
            ]).then(function(res) {
                var p = res[0], v = res[1], s = res[2];

                var pe = document.getElementById('presence-status');
                if (pe && p)
                    pe.textContent = p.present
                        ? ('Present — ' + p.count + ' person(s), confidence: ' +
                           Math.round((p.confidence || 0) * 100) + '%')
                        : 'Not present';

                var ve = document.getElementById('vitals-status');
                if (ve && v)
                    ve.textContent = v.feasible
                        ? ('Resp: ' + v.respiration_bpm + ' bpm, HR: ' +
                           v.heart_rate_bpm + ' bpm, conf: ' +
                           Math.round((v.confidence || 0) * 100) + '%')
                        : ('Unavailable: ' + (v.reason || 'n/a'));

                var he = document.getElementById('health-status');
                if (he && s)
                    he.textContent = 'Frames: ' + (s.frames_total || 0) +
                        ', rate: ' + (s.frame_rate || 0).toFixed(1) + '/s';
            });
        }, 3);

        return container;
    },
    handleSave: null,
    handleSaveApply: null,
    handleReset: null
});
