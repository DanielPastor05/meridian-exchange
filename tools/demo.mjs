import {Client,T,values,json} from './wire.mjs';
import {directory,cleanup,start} from '../tests/network_helpers.mjs';
const executable=process.argv[2];if(!executable)throw Error('Usage: node tools/demo.mjs PATH_TO_EXCHANGE_SERVER');
const dir=directory();let server,buyer,seller;
try{
 server=await start(executable,dir);buyer=await Client.connect(server.port);seller=await Client.connect(server.port);
 await buyer.login(1,'1'.repeat(32));await seller.login(2,'2'.repeat(32));
 console.log('Seller rests ten units at 100 ticks.');console.log(json(await seller.submit(1,1,100,2,100,10)));
 console.log('Buyer crosses four at a limit of 105; execution uses the maker price of 100.');
 const original=await buyer.submit(1,1,200,1,105,4);console.log(json(original));
 buyer.close();await server.stop();seller.close();
 console.log('Restart the server and retry the same request.');server=await start(executable,dir);buyer=await Client.connect(server.port);await buyer.login(1,'1'.repeat(32));
 const retry=await buyer.submit(1,1,200,1,105,4);if(json(retry)!==json(original))throw Error('retry changed outcome');console.log(json(retry));
 seller=await Client.connect(server.port);await seller.login(2,'2'.repeat(32));
 console.log('Seller kill switch cancels its remaining six units and releases the reservation.');console.log(json(await seller.submit(2,3)));
 console.log('Final book fields: changed, version, total, offset, count.');console.log(json(values((await buyer.send(T.book,Buffer.from("00000000000000000000000000000000000000000000000a","hex"))).payload)));
 console.log('PASS deterministic settlement, restart, retry and kill-switch demo');
}finally{buyer?.close();seller?.close();await server?.stop();cleanup(dir);}
