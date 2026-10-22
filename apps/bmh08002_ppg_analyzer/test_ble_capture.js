// node apps/bmh08002_ppg_analyzer/test_ble_capture.js
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const html=fs.readFileSync(__dirname+'/ble_capture.html','utf8');
const core=html.split('<script>')[1].split('// UI starts here;')[0];
const {CaptureDecoder,csvText}=vm.runInNewContext(core+';({CaptureDecoder,csvText})');
function packet(type,ms,seq){
  const b=new DataView(new ArrayBuffer(type===0x11?20:15));
  b.setUint32(0,ms);b.setUint8(4,type);b.setUint32(5,seq);
  if(type===0x11){b.setUint8(9,1);b.setUint8(11,73);b.setUint16(16,1234);b.setUint16(18,5678);}
  else{b.setInt16(9,-32768);b.setInt16(11,123);b.setInt16(13,32767);}
  return b;
}
let d=new CaptureDecoder();
const first=d.decode(packet(0x11,1000,8)),acc=d.decode(packet(0x13,1010,300));
assert.equal(first.ir_ppg,1234);assert.equal(first.red_ppg,5678);assert.equal(first.d1,73);
assert.equal(acc.acc_x,-32768);assert.equal(acc.acc_z,32767);assert.equal(acc.elapsed_s,1.01);
const gap=d.decode(packet(0x11,1084,11));assert.equal(gap.seq_gap,2);assert.equal(gap.dt_ms,84);
assert.throws(()=>d.decode(packet(0x11,1084,11)),/duplicate/);
assert.throws(()=>d.decode(packet(0x11,10,0)),/reset/);
assert.throws(()=>d.decode(new DataView(new ArrayBuffer(3))),/length/);
assert.throws(()=>d.decode(packet(0x12,100,0)),/type/);
const invalid=packet(0x11,1112,12);invalid.setUint8(9,3);assert.throws(()=>d.decode(invalid),/status/);
assert.equal(d.decode(packet(0x11,1112,12)).seq_gap,0); // invalid didn't advance tracker
d=new CaptureDecoder();
d.decode(packet(0x11,0xfffffff0,0xffffffff));d.decode(packet(0x13,0xfffffff8,50));
assert.equal(d.decode(packet(0x11,12,0)).elapsed_s,4294967.308);
assert.equal(d.decode(packet(0x13,30,51)).elapsed_s,4294967.326);
const csv=csvText([{sample:first,activity:'walk,"test"',host_elapsed_s:0,host_unix_ms:1}], 'ppg');
assert(csv.includes('ir_ppg,red_ppg,status,d1'));assert(csv.includes('"walk,""test"""'));
assert(!csvText([{error:'bad'}],'acc').includes('bad'));
console.log('BLE decode/export checks passed: signed ACC, independent sequences, gaps, wrap, reset, malformed frames, CSV escaping.');
