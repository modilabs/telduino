bool initSPI()
{
        //Change SPI speed/endianness as empirically determined. -JR    
}

bool readData(int8_t numBits, int8_t regAddr, uint8_t[3] data)
{
        int8_t numBytes = (numBits+7)/8;
        int8_t ii = 0;
        if (regAddr & 0b11000000) {
                //error: not a read instruction -AM
                return false;
        }

        //now transfer the readInstuction/registerAddress: i.e. 00xxxxxx -AM
        SPI.transfer(regAddr);

        //initialize data to be all zeros first -AM
        for (ii=0; ii<3; ii++) {
                uint8_t[ii] = 0x00;
        }

        //now read the data on the SPI data register byte-by-byte with the MSB first - AM
        for (ii=0; ii<numBytes; ii++) {

                uint8_t[ii] = SPI.transfer(0x00);
        }

        //make sure that the data buffer is properly organized -AM
        //ans: it is -AM


        return true;
}

