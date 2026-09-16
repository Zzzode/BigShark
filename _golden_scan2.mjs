import fs from 'fs';
const fx=JSON.parse(fs.readFileSync('platforms/river-club/tests/fixtures/v0-golden.json','utf8'));
const combos={}; let preBet=0;
const rows=[];
for(const g of fx){
  const room=g.state?.room ?? g.room ?? g;
  if(!room?.events) continue;
  for(const ev of room.events){
    combos[room.street+':'+ev.kind]=(combos[room.street+':'+ev.kind]||0)+1;
    if(ev.kind==='bet') rows.push({fixture:g.name, snapshotStreet:room.street, text:ev.text, call:room.legal?.call, actions:room.legal?.actions});
    if(room.street==='preflop'&&ev.kind==='bet') preBet++;
  }
}
console.log('combos:',combos);
console.log('preflop-snapshot bet events:',preBet);
console.log('all bet events across fixtures:'); console.log(JSON.stringify(rows,null,2));
