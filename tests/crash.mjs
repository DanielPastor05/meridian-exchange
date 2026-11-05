import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {Client,T,values,decodeOutcome} from '../tools/wire.mjs';
import {directory,cleanup,start} from './network_helpers.mjs';
const executable=process.argv[2];if(!executable)throw Error('server executable required');
const report=[];
for(const stage of ['before_append','mid_append','after_write','after_sync','after_apply','before_response','after_response']){
 const dir=directory();let server,client,seller;
 try{
  // Establish an acknowledged prefix before targeting the second command.
  server=await start(executable,dir);client=await Client.connect(server.port);await client.login(1,'1'.repeat(32));
  const prefix=await client.submit(1,1,10,1,90,7);
  seller=await Client.connect(server.port);await seller.login(2,'2'.repeat(32));
  await seller.submit(1,1,20,2,100,10);seller.close();client.close();await server.stop();
  server=await start(executable,dir,['--fault-stage',stage,'--fault-account','1','--fault-request','2']);
  client=await Client.connect(server.port);await client.login(1,'1'.repeat(32));
  let acknowledgement=null;
  const pending=client.submit(2,1,11,1,105,3).then(r=>{acknowledgement=r;return r;},()=>null);
  await server.waitFor('fault_ready');
  if(stage==='after_response'){await pending;assert(acknowledgement,'after_response did not deliver an acknowledgement');}
  await server.stop();await pending;client.close();
  const tornSize=fs.statSync(path.join(dir,'server.journal')).size;
  server=await start(executable,dir);client=await Client.connect(server.port);await client.login(1,'1'.repeat(32));
  const before=values((await client.send(T.account)).payload);
  assert(before[1]===1n||before[1]===2n);if(['before_append','mid_append'].includes(stage))assert.equal(before[1],1n,'uncommitted target survived');assert(before[2]>=prefix.globalSequence,'acknowledged prefix disappeared');
  if(['after_sync','after_apply','before_response','after_response'].includes(stage))assert.equal(before[1],2n,'durable command missing');
  const outcome=await client.submit(2,1,11,1,105,3);assert.equal(outcome.status,'accepted');
  if(acknowledgement)assert.deepEqual(outcome,acknowledgement,'acknowledged result changed after restart');
  const retry=await client.submit(2,1,11,1,105,3);assert.deepEqual(retry,outcome);
  const state=values((await client.send(T.account)).payload);assert.equal(state[2],3n,'retry executed twice');
  assert.equal(state[6],7n*90n,'recovery lost reservations');
  assert.equal(state[4],999999700n);assert.equal(state[5],100003n);
  seller=await Client.connect(server.port);await seller.login(2,'2'.repeat(32));
  const maker=values((await seller.send(T.account)).payload);
  assert.equal(maker[4],1000000300n);assert.equal(maker[5],99997n);assert.equal(maker[7],7n);seller.close();
  assert.equal(outcome.filled,3n);assert.equal(outcome.executions,1n);
  report.push({stage,externalTermination:true,acknowledged:!!acknowledgement,sequenceBeforeRetry:before[1].toString(),sequenceAfterRetry:state[2].toString(),journalBytesAtCrash:tornSize,result:'pass'});
  console.log('PASS external process termination at '+stage);
 }finally{seller?.close();client?.close();await server?.stop();cleanup(dir);}
}
if(process.argv[3])fs.writeFileSync(process.argv[3],JSON.stringify(report,null,2)+'\n');
