#ifndef __BITMAP_INFOMATION_H__
#define __BITMAP_INFOMATION_H__

class BitmapInfomation {
public:
	BitmapInfomation( tjs_uint width, tjs_uint height, int bpp, bool unpadding=false );
	virtual ~BitmapInfomation();

	inline unsigned int GetBPP() const { return BitmapInfo->bmiHeader.biBitCount; }
	inline bool Is32bit() const { return GetBPP() == 32; }
	inline bool Is8bit() const { return GetBPP() == 8; }
	inline int GetWidth() const { return BitmapInfo->bmiHeader.biWidth; }
	inline int GetHeight() const { return BitmapInfo->bmiHeader.biHeight; }
	inline bool GetUnpadding() const { return unpadding; };
	inline tjs_uint GetImageSize() const { return BitmapInfo->bmiHeader.biSizeImage; }
	inline int GetPitchBytes() const { return GetImageSize()/GetHeight(); }

	const BITMAPINFO* GetBITMAPINFO() const { return BitmapInfo; }
	const BITMAPINFOHEADER * GetBITMAPINFOHEADER() const { 
		return (const BITMAPINFOHEADER*)(BitmapInfo);
	}
private:
	BITMAPINFO* BitmapInfo;
	tjs_int BitmapInfoSize;
	bool unpadding;
};

BitmapInfomation *TVPCreateBitmapInfo(tjs_uint width, tjs_uint height, int bpp, bool unpadding=false);

#endif // __BITMAP_INFOMATION_H__

