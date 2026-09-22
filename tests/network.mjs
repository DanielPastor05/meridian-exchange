import assert from 'node:assert/strict';
import net from 'node:net';
import {Client,T,frame,hello,ints,values,request,decodeOutcome} from '../tools/wire.mjs';
import {directory,cleanup,start,delay} from './network_helpers.mjs';
const executable=process.argv[2];if(!executable)throw Error('server executable required');
const dir=directory();let server,one,two,three;const sockets=[];
try{
 server=await start(executable,dir,['--durability','buffered','--timeout-ms','5000','--max-clients','16']);
 one=await Client.connect(server.port);two=await Client.connect(server.port);three=await Client.connect(server.port);
 await one.login(1,'1'.repeat(32),1);await two.login(2,'2'.repeat(32));await three.login(3,'3'.repeat(32));
 const sell=await one.submit(1,1,10,2,100,10);assert.equal(sell.status,'accepted');
 const buy=await two.submit(1,1,20,1,100,4);assert.equal(buy.filled,4n);assert.equal(buy.cash,999999600n);
 assert.deepEqual(await one.submit(1,1,10,2,100,10),sell);
 assert.equal((await one.submit(1,1,10,2,100,11)).status,'conflict');
 assert.equal((await two.submit(3,2,10)).status,'gap');
 assert.equal((await two.submit(2,2,10)).status,'not_owner');
 assert.equal((await one.submit(2,3)).status,'killed');
 assert.equal((await one.submit(3,1,30,1,90,1)).status,'halted');
 assert.equal((await one.submit(4,4)).status,'resumed');
 console.log('CHECK Separate clients');
 // Separate clients submit concurrently into one serialized book.
 const replies=await Promise.all([one.submit(5,1,31,1,90,5),two.submit(3,1,32,2,110,5),three.submit(1,1,33,1,89,5)]);
 assert(replies.every(r=>r.status==='accepted'));assert.equal(new Set(replies.map(r=>r.globalSequence)).size,3);
 const book=values((await one.send(T.book,ints(0,0,256))).payload);assert.equal(book[2],3n);const version=book[1];
 await one.submit(6,2,31);
 assert.equal(values((await one.send(T.book,ints(1,version,256))).payload)[0],1n);
 const events=values((await one.send(T.events,ints(0,256))).payload);assert.equal(events[0],0n);assert(events[6]>0);
 const bad=await Client.connect(server.port);await assert.rejects(bad.login(1,'f'.repeat(32)));bad.close();
 const unauth=await Client.connect(server.port);assert.equal((await unauth.send(T.account)).type,T.error);unauth.close();
 console.log('CHECK A malformed frame');
 // A malformed frame must close only its own connection, without advancing the log.
 const malformed=await Client.connect(server.port);await malformed.login(1,'1'.repeat(32));
 await assert.rejects(malformed.send(T.submit,request(7,999,1,1,100,1)));malformed.close();
 const oversized=net.createConnection({port:server.port,host:'127.0.0.1'});sockets.push(oversized);oversized.on('error',()=>{});
 await new Promise(resolve=>oversized.once('connect',resolve));const invalid=frame(T.hello);invalid.writeUInt32BE(65537,8);oversized.write(invalid);
 await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('oversized frame not closed')),3000);oversized.once('close',()=>{clearTimeout(timer);resolve();});});
 // EOF inside a header and an idle partial frame must not stop healthy clients.
 const partial=net.createConnection({port:server.port,host:'127.0.0.1'});sockets.push(partial);partial.on('error',()=>{});
 await new Promise(resolve=>partial.once('connect',resolve));partial.write(Buffer.from('MDX'));partial.end();
 assert.equal((await one.send(T.ping)).type,T.ping|0x8000);
 console.log('CHECK Fill the event ring');
 // Fill the event ring, then require an explicit quote snapshot on a stale cursor.
 for(let i=0;i<8300;i++){await three.submit(2+i,2,999999);if(i%100===0)await Promise.all([one.send(T.ping),two.send(T.ping)]);}
 const gap=values((await one.send(T.events,ints(0,256))).payload);assert.equal(gap[0],1n);assert.equal(gap[6],0n);
 const latest=gap[1];await two.submit(4,2,32);
 const after=values((await one.send(T.events,ints(latest,256))).payload);assert.equal(after[0],0n);assert.equal(after[6],1n);
 console.log('CHECK A paused socket');
 // A paused socket pipelines queries until its output window fills. Writes occur outside the book lock.
 const slow=net.createConnection({port:server.port,host:'127.0.0.1'});sockets.push(slow);slow.on('error',()=>{});
 const slowClosed=new Promise(resolve=>slow.once('close',resolve));
 await new Promise(resolve=>slow.once('connect',resolve));slow.write(frame(T.hello,hello(1,'1'.repeat(32))));slow.pause();
 const query=frame(T.events,ints(latest-256n,256));slow.write(Buffer.concat(Array.from({length:1200},()=>query)));
 for(let i=0;i<10;i++)await one.send(T.ping);
 console.log('CHECK healthy ping after slow consumer');
 for(let i=0;i<20;i++){await delay(300);await one.send(T.ping);}
 let pingFailure;const keepAlive=setInterval(()=>{one.send(T.ping).catch(error=>{pingFailure=error;});},300);
 let closeTimer;try{slow.resume();await Promise.race([slowClosed,new Promise((_,reject)=>{closeTimer=setTimeout(()=>reject(Error('slow reader was not evicted')),7000);})]);}
 finally{clearInterval(keepAlive);clearTimeout(closeTimer);}if(pingFailure)throw pingFailure;
 const metrics=values((await one.send(T.metrics)).payload);assert(metrics[3]>=1n);assert(metrics[4]>=1n);
 one.close();two.close();three.close();sockets.forEach(s=>s.destroy());await server.stop(process.platform==='win32'?'SIGKILL':'SIGTERM');
 server=await start(executable,dir,['--durability','sync']);one=await Client.connect(server.port);await one.login(2,'2'.repeat(32));
 const replay=await one.submit(4,2,32);assert.equal(replay.status,'cancelled');
 const state=values((await one.send(T.account)).payload);assert.equal(state[1],4n);
 one.close();await server.stop(process.platform==='win32'?'SIGKILL':'SIGTERM');
 server=await start(executable,dir,['--max-clients','1','--timeout-ms','2000']);
 one=await Client.connect(server.port);await one.login(1,'1'.repeat(32));
 const excess=await Client.connect(server.port);await assert.rejects(excess.login(2,'2'.repeat(32)));excess.close();
 assert.equal((await one.send(T.ping)).type,T.ping|0x8000);one.close();await delay(100);
 const idle=net.createConnection({port:server.port,host:'127.0.0.1'});sockets.push(idle);idle.on('error',()=>{});
 await new Promise(resolve=>idle.once('connect',resolve));idle.write(Buffer.from('MDX'));
 await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('incomplete frame was not evicted')),5000);idle.once('close',()=>{clearTimeout(timer);resolve();});});
 await delay(100);one=await Client.connect(server.port);await one.login(1,'1'.repeat(32));await one.send(T.ping);
 console.log('PASS connection cap and incomplete-frame deadline');
 console.log('PASS TCP sessions, fragmentation, ownership, concurrency, retries, reconnect, malformed input, feed recovery and slow-reader isolation');
}finally{one?.close();two?.close();three?.close();sockets.forEach(s=>s.destroy());await server?.stop(process.platform==='win32'?'SIGKILL':'SIGTERM');cleanup(dir);}
