import net from 'node:net';
export const T={hello:1,submit:2,account:3,events:4,book:5,ping:6,metrics:7,error:65535};
export const codes=['accepted','cancelled','invalid','duplicate_order','unknown_order','capacity','stale','gap','conflict',
 'unknown_account','not_owner','halted','order_limit','funds','inventory','exposure','position','overflow','self_trade','killed','resumed'];
export function ints(...values){const b=Buffer.alloc(values.length*8);values.forEach((v,i)=>b.writeBigUInt64BE(BigInt.asUintN(64,BigInt(v)),i*8));return b;}
export function values(payload){if(payload.length%8)throw Error('unaligned response');return Array.from({length:payload.length/8},(_,i)=>payload.readBigUInt64BE(i*8));}
export function frame(type,payload=Buffer.alloc(0)){
 if(payload.length>65536)throw Error('frame exceeds 64 KiB');const h=Buffer.alloc(16);h.write('MDX1');h.writeUInt16BE(1,4);
 h.writeUInt16BE(type,6);h.writeUInt32BE(payload.length,8);return Buffer.concat([h,payload]);
}
export function hello(account,token){if(!/^[0-9a-f]{32}$/.test(token))throw Error('token must be 32 lowercase hex characters');return Buffer.concat([ints(account),Buffer.from(token)]);}
export function request(sequence,kind,id=0,side=1,price=0,quantity=0){return ints(sequence,kind,id,side,price,quantity);}
export function decodeOutcome(payload){
 const v=values(payload);if(v.length!==19)throw Error('invalid outcome length');
 const names=['globalSequence','requestSequence','code','remaining','filled','executions','cancelled','eventSequence',
 'cash','inventory','reservedCash','reservedInventory','buyQuantity','openNotional','halted','bid','ask','bidQuantity','askQuantity'];
 const out=Object.fromEntries(names.map((n,i)=>[n,v[i]]));out.status=codes[Number(out.code)]??'invalid';return out;
}
export function json(value){return JSON.stringify(value,(_,v)=>typeof v==='bigint'?v.toString():v,2);}
export class Client{
 constructor(socket,timeout=5000){
  this.socket=socket;this.timeout=timeout;this.pending=[];this.buffer=Buffer.alloc(0);this.failure=null;
  socket.setNoDelay(true);socket.on('data',chunk=>{
   try{
    this.buffer=Buffer.concat([this.buffer,chunk]);
    while(this.buffer.length>=16){
     if(this.buffer.toString('ascii',0,4)!=='MDX1'||this.buffer.readUInt16BE(4)!==1||this.buffer.readUInt32BE(12)!==0)throw Error('invalid response header');
     const size=this.buffer.readUInt32BE(8);if(size>65536)throw Error('oversized response');if(this.buffer.length<16+size)break;
     const type=this.buffer.readUInt16BE(6),payload=Buffer.from(this.buffer.subarray(16,16+size));this.buffer=this.buffer.subarray(16+size);
     const pending=this.pending.shift();if(!pending)throw Error('unsolicited response');clearTimeout(pending.timer);pending.resolve({type,payload});
    }
   }catch(error){this.fail(error);}
  });
  socket.on('error',error=>this.fail(error));socket.on('close',()=>this.fail(Error('connection closed')));
 }
 static async connect(port,host='127.0.0.1',timeout=5000){
  const socket=net.createConnection({port,host});
  const client=new Client(socket,timeout);
  await new Promise((resolve,reject)=>{const timer=setTimeout(()=>{socket.destroy();reject(Error('connect deadline'));},timeout);
   socket.once('connect',()=>{clearTimeout(timer);resolve();});socket.once('error',e=>{clearTimeout(timer);reject(e);});});return client;
 }
 fail(error){if(!this.failure)this.failure=error;for(const p of this.pending){clearTimeout(p.timer);p.reject(error);}this.pending=[];this.socket.destroy();}
 send(type,payload=Buffer.alloc(0),fragment=0){
  if(this.failure)return Promise.reject(this.failure);if(this.pending.length>=4096)return Promise.reject(Error('client pending limit'));
  const bytes=frame(type,payload);
  return new Promise((resolve,reject)=>{
   const timer=setTimeout(()=>this.fail(Error('response deadline')),this.timeout);this.pending.push({resolve,reject,timer});
   if(fragment>0){for(let i=0;i<bytes.length;i+=fragment)this.socket.write(bytes.subarray(i,i+fragment));}
   else this.socket.write(bytes);
  });
 }
 async login(account,token,fragment=0){const reply=await this.send(T.hello,hello(account,token),fragment);if(reply.type===T.error)throw Error('authentication rejected');return values(reply.payload);}
 async submit(...args){const reply=await this.send(T.submit,request(...args));if(reply.type===T.error)throw Error('protocol rejected');return decodeOutcome(reply.payload);}
 close(){this.socket.destroy();}
}
