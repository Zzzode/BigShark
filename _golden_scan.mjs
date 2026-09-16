import fs from 'fs';
const fx=JSON.parse(fs.readFileSync('platforms/river-club/tests/fixtures/v0-golden.json','utf8'));
console.log('golden fixtures:', fx.length);
let preBet=0, preAggressive=0; const streets={};
for(const g of fx){
  const room=g.room??g;
  if(!room||!Array.isArray(room.events)) continue;
  streets[room.street]=(streets[room.street]||0)+1;
  for(const ev of room.events){
    if(room.street==='preflop' && (ev.kind==='bet'||ev.kind==='raise')) preAggressive++;
    if(room.street==='preflop' && ev.kind==='bet'){preBet++; console.log('PREFLOP BET:', JSON.stringify({text:ev.text,call:room.legal?.call,actions:room.legal?.actions}));}
  }
}
console.log('fixture streets:', streets);
console.log('preflop bet events:', preBet, ' preflop bet/raise events:', preAggressive);
// also list every distinct (street, kind) and any bet anywhere with its street-at-snapshot
const combos={};
for(const g of fx){const room=g.room??g; if(!room?.events)continue; for(const ev of room.events){const k=room.street+':'+ev.kind; combos[k]=(combos[k]||0)+1;}}
console.log('snapshot-street:event-kind combos:', combos);
// show the shape of one preflop fixture's events + legal
const pre=fx.map(g=>g.room??g).find(r=>r.street==='preflop');
if(pre) console.log('sample preflop:', JSON.stringify({events:pre.events, legal:pre.legal, seats:(pre.seats||[]).filter(Boolean).map(s=>({n:s.name,bet:s.bet,blind:s.blind,status:s.status}))},null,2));
