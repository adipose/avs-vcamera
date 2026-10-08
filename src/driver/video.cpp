/**************************************************************************

    AVStream Filter-Centric Sample

    Copyright (c) 1999 - 2001, Microsoft Corporation

    File:

        video.cpp

    Abstract:

        This file contains the video capture pin implementation.

    History:

        created 6/11/01

**************************************************************************/

#include "avssamp.h"

/**************************************************************************

    PAGEABLE CODE

**************************************************************************/

#ifdef ALLOC_PRAGMA
#pragma code_seg("PAGE")
#endif // ALLOC_PRAGMA

NTSTATUS
CVideoCapturePin::
DispatchCreate (
    IN PKSPIN Pin,
    IN PIRP Irp
    )

/*++

Routine Description:

    Create a new video capture pin.  This is the creation dispatch for
    the video capture pin.

Arguments:

    Pin -
        The pin being created

    Irp -
        The creation Irp

Return Value:

    Success / Failure

--*/

{

    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;

    CVideoCapturePin *CapPin = new (NonPagedPool) CVideoCapturePin (Pin);
    CCapturePin *BasePin = static_cast <CCapturePin *> (CapPin);

    if (!CapPin) {
        //
        // Return failure if we couldn't create the pin.
        //
        Status = STATUS_INSUFFICIENT_RESOURCES;

    } else {
        //
        // Add the item to the object bag if we we were successful. 
        // Whenever the pin closes, the bag is cleaned up and we will be
        // freed.
        //
        Status = KsAddItemToObjectBag (
            Pin -> Bag,
            reinterpret_cast <PVOID> (BasePin),
            reinterpret_cast <PFNKSFREE> (CCapturePin::BagCleanup)
            );

        if (!NT_SUCCESS (Status)) {
            delete CapPin;
        } else {
            Pin -> Context = reinterpret_cast <PVOID> (BasePin);
        }

    }

    //
    // If we succeeded so far, stash the video info header away and change
    // our allocator framing to reflect the fact that only now do we know
    // the framing requirements based on the connection format.
    //
    PKS_VIDEOINFOHEADER VideoInfoHeader = NULL;

    if (NT_SUCCESS (Status)) {

        VideoInfoHeader = CapPin -> CaptureVideoInfoHeader ();
        if (!VideoInfoHeader) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if (NT_SUCCESS (Status)) {
        
        //
        // We need to edit the descriptor to ensure we don't mess up any other
        // pins using the descriptor or touch read-only memory.
        //
        Status = KsEdit (Pin, &Pin -> Descriptor, 'aChS');

        if (NT_SUCCESS (Status)) {
            Status = KsEdit (
                Pin, 
                &(Pin -> Descriptor -> AllocatorFraming),
                'aChS'
                );
        }

        //
        // If the edits proceeded without running out of memory, adjust 
        // the framing based on the video info header.
        //
        if (NT_SUCCESS (Status)) {

            //
            // We've KsEdit'ed this...  I'm safe to cast away constness as
            // long as the edit succeeded.
            //
            PKSALLOCATOR_FRAMING_EX Framing =
                const_cast <PKSALLOCATOR_FRAMING_EX> (
                    Pin -> Descriptor -> AllocatorFraming
                    );

            Framing -> FramingItem [0].Frames = 2;

            //
            // The physical and optimal ranges must be biSizeImage.  We only
            // support one frame size, precisely the size of each capture
            // image.
            //
            Framing -> FramingItem [0].PhysicalRange.MinFrameSize =
                Framing -> FramingItem [0].PhysicalRange.MaxFrameSize =
                Framing -> FramingItem [0].FramingRange.Range.MinFrameSize =
                Framing -> FramingItem [0].FramingRange.Range.MaxFrameSize =
                VideoInfoHeader -> bmiHeader.biSizeImage;

            Framing -> FramingItem [0].PhysicalRange.Stepping = 
                Framing -> FramingItem [0].FramingRange.Range.Stepping =
                0;

        }

    }

    if (NT_SUCCESS (Status)) {
        //
        // Adjust the stream header size.  The video packets have extended
        // header info (KS_FRAME_INFO).
        //
        Pin -> StreamHeaderSize = sizeof (KSSTREAM_HEADER) +
            sizeof (KS_FRAME_INFO);

    }

    return Status;

}

/*************************************************/


PKS_VIDEOINFOHEADER 
CVideoCapturePin::
CaptureVideoInfoHeader (
    )

/*++

Routine Description:

    Capture the video info header out of the connection format.  This
    is what we use to base synthesized images off.

Arguments:

    None

Return Value:

    The captured video info header or NULL if there is insufficient
    memory.

--*/

{

    PAGED_CODE();

    PKS_VIDEOINFOHEADER ConnectionHeader =
        &((reinterpret_cast <PKS_DATAFORMAT_VIDEOINFOHEADER> 
            (m_Pin -> ConnectionFormat)) -> 
            VideoInfoHeader);

    m_VideoInfoHeader = reinterpret_cast <PKS_VIDEOINFOHEADER> (
        ExAllocatePoolZero (
            NonPagedPool,
            KS_SIZE_VIDEOHEADER (ConnectionHeader),
            AVSSMP_POOLTAG
            )
        );

    if (!m_VideoInfoHeader)
        return NULL;

    //
    // Bag the newly allocated header space.  This will get cleaned up
    // automatically when the pin closes.
    //
    NTSTATUS Status =
        KsAddItemToObjectBag (
            m_Pin -> Bag,
            reinterpret_cast <PVOID> (m_VideoInfoHeader),
            NULL
            );

    if (!NT_SUCCESS (Status)) {

        ExFreePool (m_VideoInfoHeader);
        return NULL;

    } else {

        //
        // Copy the connection format video info header into the newly 
        // allocated "captured" video info header.
        //
        RtlCopyMemory (
            m_VideoInfoHeader,
            ConnectionHeader,
            KS_SIZE_VIDEOHEADER (ConnectionHeader)
            );

    }

    return m_VideoInfoHeader;

}

/*************************************************/


NTSTATUS
CVideoCapturePin::
IntersectHandler (
    IN PKSFILTER Filter,
    IN PIRP Irp,
    IN PKSP_PIN PinInstance,
    IN PKSDATARANGE CallerDataRange,
    IN PKSDATARANGE DescriptorDataRange,
    IN ULONG BufferSize,
    OUT PVOID Data OPTIONAL,
    OUT PULONG DataSize
    )

/*++

Routine Description:

    This routine handles video pin intersection queries by determining the
    intersection between two data ranges.

Arguments:

    Filter -
        Contains a void pointer to the  filter structure.

    Irp -
        Contains a pointer to the data intersection property request.

    PinInstance -
        Contains a pointer to a structure indicating the pin in question.

    CallerDataRange -
        Contains a pointer to one of the data ranges supplied by the client
        in the data intersection request.  The format type, subtype and
        specifier are compatible with the DescriptorDataRange.

    DescriptorDataRange -
        Contains a pointer to one of the data ranges from the pin descriptor
        for the pin in question.  The format type, subtype and specifier are
        compatible with the CallerDataRange.

    BufferSize -
        Contains the size in bytes of the buffer pointed to by the Data
        argument.  For size queries, this value will be zero.

    Data -
        Optionally contains a pointer to the buffer to contain the data 
        format structure representing the best format in the intersection 
        of the two data ranges.  For size queries, this pointer will be 
        NULL.

    DataSize -
        Contains a pointer to the location at which to deposit the size 
        of the data format.  This information is supplied by the function 
        when the format is actually delivered and in response to size 
        queries.

Return Value:

    STATUS_SUCCESS if there is an intersection and it fits in the supplied
    buffer, STATUS_BUFFER_OVERFLOW for successful size queries, 
    STATUS_NO_MATCH if the intersection is empty, or 
    STATUS_BUFFER_TOO_SMALL if the supplied buffer is too small.

--*/

{
    PAGED_CODE();

    const GUID VideoInfoSpecifier = 
        {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_VIDEOINFO)};
    
    NT_ASSERT(Filter);
    NT_ASSERT(Irp);
    NT_ASSERT(PinInstance);
    NT_ASSERT(CallerDataRange);
    NT_ASSERT(DescriptorDataRange);
    NT_ASSERT(DataSize);
    
    ULONG DataFormatSize;
    
    //
    // Specifier FORMAT_VideoInfo for VIDEOINFOHEADER
    //
    if (IsEqualGUID(CallerDataRange->Specifier, VideoInfoSpecifier) &&
        CallerDataRange->FormatSize >= sizeof (KS_DATARANGE_VIDEO)) {
            
        PKS_DATARANGE_VIDEO callerDataRange = 
            reinterpret_cast <PKS_DATARANGE_VIDEO> (CallerDataRange);

        PKS_DATARANGE_VIDEO descriptorDataRange = 
            reinterpret_cast <PKS_DATARANGE_VIDEO> (DescriptorDataRange);

        PKS_DATAFORMAT_VIDEOINFOHEADER FormatVideoInfoHeader;

        //
        // Check that the other fields match
        //
        if ((callerDataRange->bFixedSizeSamples != 
                descriptorDataRange->bFixedSizeSamples) ||
            (callerDataRange->bTemporalCompression != 
                descriptorDataRange->bTemporalCompression) ||
            (callerDataRange->StreamDescriptionFlags != 
                descriptorDataRange->StreamDescriptionFlags) ||
            (callerDataRange->MemoryAllocationFlags != 
                descriptorDataRange->MemoryAllocationFlags) ||
            (RtlCompareMemory (&callerDataRange->ConfigCaps,
                    &descriptorDataRange->ConfigCaps,
                    sizeof (KS_VIDEO_STREAM_CONFIG_CAPS)) != 
                    sizeof (KS_VIDEO_STREAM_CONFIG_CAPS))) 
        {
            return STATUS_NO_MATCH;
        }

        //
        // KS_SIZE_VIDEOHEADER() below is relying on bmiHeader.biSize from
        // the caller's data range.  This **MUST** be validated; the
        // extended bmiHeader size (biSize) must not extend past the end
        // of the range buffer.  Possible arithmetic overflow is also
        // checked for.
        //
        {
            ULONG VideoHeaderSize = KS_SIZE_VIDEOHEADER (
                &callerDataRange->VideoInfoHeader
                );

            ULONG DataRangeSize = 
                FIELD_OFFSET (KS_DATARANGE_VIDEO, VideoInfoHeader) +
                VideoHeaderSize;

            //
            // Check that biSize does not extend past the buffer.  The 
            // first two checks are for arithmetic overflow on the 
            // operations to compute the alleged size.  (On unsigned
            // math, a+b < a iff an arithmetic overflow occurred).
            //
            if (
                VideoHeaderSize < callerDataRange->
                    VideoInfoHeader.bmiHeader.biSize ||
                DataRangeSize < VideoHeaderSize ||
                DataRangeSize > callerDataRange -> DataRange.FormatSize
                ) {

                return STATUS_INVALID_PARAMETER;

            }

        }

        DataFormatSize = 
            sizeof (KSDATAFORMAT) + 
            KS_SIZE_VIDEOHEADER (&callerDataRange->VideoInfoHeader);
            
        //
        // If the passed buffer size is 0, it indicates that this is a size
        // only query.  Return the size of the intersecting data format and
        // pass back STATUS_BUFFER_OVERFLOW.
        //
        if (BufferSize == 0) {

            *DataSize = DataFormatSize;
            return STATUS_BUFFER_OVERFLOW;

        }
        
        //
        // Verify that the provided structure is large enough to
        // accept the result.
        //
        if (BufferSize < DataFormatSize) 
        {
            return STATUS_BUFFER_TOO_SMALL;
        }

        //
        // Copy over the KSDATAFORMAT, followed by the actual VideoInfoHeader
        //
        *DataSize = DataFormatSize;
            
        FormatVideoInfoHeader = PKS_DATAFORMAT_VIDEOINFOHEADER( Data );

        //
        // Copy over the KSDATAFORMAT.  This is precisely the same as the
        // KSDATARANGE (it's just the GUIDs, etc...  not the format information
        // following any data format.
        // 
        RtlCopyMemory (
            &FormatVideoInfoHeader->DataFormat, 
            DescriptorDataRange, 
            sizeof (KSDATAFORMAT));

        FormatVideoInfoHeader->DataFormat.FormatSize = DataFormatSize;

        //
        // Copy over the callers requested VIDEOINFOHEADER
        //

        RtlCopyMemory (
            &FormatVideoInfoHeader->VideoInfoHeader, 
            &callerDataRange->VideoInfoHeader,
            KS_SIZE_VIDEOHEADER (&callerDataRange->VideoInfoHeader) 
            );

        //
        // Calculate biSizeImage for this request, and put the result in both
        // the biSizeImage field of the bmiHeader AND in the SampleSize field
        // of the DataFormat.
        //
        // Note that for compressed sizes, this calculation will probably not
        // be just width * height * bitdepth
        //
        FormatVideoInfoHeader->VideoInfoHeader.bmiHeader.biSizeImage =
            FormatVideoInfoHeader->DataFormat.SampleSize = 
            KS_DIBSIZE (FormatVideoInfoHeader->VideoInfoHeader.bmiHeader);

        //
        // REVIEW - Perform other validation such as cropping and scaling checks
        // 
        
        return STATUS_SUCCESS;
        
    } // End of VIDEOINFOHEADER specifier
    
    return STATUS_NO_MATCH;
}

/*************************************************/

BOOL
MultiplyCheckOverflow (
    ULONG a,
    ULONG b,
    ULONG *pab
    )

/*++

Routine Description:

    Perform a 32 bit unsigned multiplication and check for arithmetic overflow.

Arguments:

    a -
        First operand

    b -
        Second operand

    pab -
        Result

Return Value:

    TRUE -
        no overflow

    FALSE -
        overflow occurred

--*/

{
    PAGED_CODE();

    *pab = a * b;
    if ((a == 0) || (((*pab) / a) == b)) {
        return TRUE;
    }
    return FALSE;
}

/*************************************************/


NTSTATUS
CVideoCapturePin::
DispatchSetFormat (
    IN PKSPIN Pin,
    IN PKSDATAFORMAT OldFormat OPTIONAL,
    IN PKSMULTIPLE_ITEM OldAttributeList OPTIONAL,
    IN const KSDATARANGE *DataRange,
    IN const KSATTRIBUTE_LIST *AttributeRange OPTIONAL
    )

/*++

Routine Description:

    This is the set data format dispatch for the capture pin.  It is called
    in two circumstances.

        1: before Pin's creation dispatch has been made to verify that
           Pin -> ConnectionFormat is an acceptable format for the range
           DataRange.  In this case OldFormat is NULL.

        2: after Pin's creation dispatch has been made and an initial format
           selected in order to change the format for the pin.  In this case,
           OldFormat will not be NULL.

    Validate that the format is acceptable and perform the actions necessary
    to change format if appropriate.

Arguments:

    Pin -
        The pin this format is being set on.  The format itself will be in
        Pin -> ConnectionFormat.

    OldFormat -
        The previous format used on this pin.  If this is NULL, it is an
        indication that Pin's creation dispatch has not yet been made and
        that this is a request to validate the initial format and not to
        change formats.

    OldAttributeList -
        The old attribute list for the prior format

    DataRange -
        A range out of our list of data ranges which was determined to be
        at least a partial match for Pin -> ConnectionFormat.  If the format
        there is unacceptable for the range, STATUS_NO_MATCH should be
        returned.

    AttributeRange -
        The attribute range

Return Value:

    Success / Failure

        STATUS_SUCCESS -
            The format is acceptable / the format has been changed

        STATUS_NO_MATCH -
            The format is not-acceptable / the format has not been changed

--*/

{

    PAGED_CODE();

    NTSTATUS Status = STATUS_NO_MATCH;

    const GUID VideoInfoSpecifier = 
        {STATICGUIDOF(KSDATAFORMAT_SPECIFIER_VIDEOINFO)};

    CCapturePin *CapPin = NULL;
    CVideoCapturePin *VidCapPin = NULL;

    //
    // Find the pin, if it exists yet.  OldFormat will be an indication of 
    // this.  If we're changing formats, OldFormat will be non-NULL.
    //
    // You cannot use Pin -> Context to make the determination.  AVStream
    // preinitializes this to the filter's context.
    //
    if (OldFormat) {
        CapPin = reinterpret_cast <CCapturePin *> (Pin -> Context);

        //
        // We know this pin happens to be the video capture pin.  Downcast it.
        //
        VidCapPin = static_cast <CVideoCapturePin *> (CapPin);
    }

    if (IsEqualGUID (Pin -> ConnectionFormat -> Specifier,
            VideoInfoSpecifier) &&
        Pin -> ConnectionFormat -> FormatSize >= 
            sizeof (KS_DATAFORMAT_VIDEOINFOHEADER)
        ) {

        PKS_DATAFORMAT_VIDEOINFOHEADER ConnectionFormat =
            reinterpret_cast <PKS_DATAFORMAT_VIDEOINFOHEADER> 
                (Pin -> ConnectionFormat);

        //
        // DataRange comes out of OUR data range list.  I know the range
        // is valid as such.
        //
        const KS_DATARANGE_VIDEO *VIRange =
            reinterpret_cast <const KS_DATARANGE_VIDEO *>
                (DataRange);

        //
        // Check that bmiHeader.biSize is valid since we use it later.
        //
        ULONG VideoHeaderSize = KS_SIZE_VIDEOHEADER (
            &ConnectionFormat -> VideoInfoHeader
            );

        ULONG DataFormatSize = FIELD_OFFSET (
            KS_DATAFORMAT_VIDEOINFOHEADER, VideoInfoHeader
            ) + VideoHeaderSize;

        if (
            VideoHeaderSize < ConnectionFormat->
                VideoInfoHeader.bmiHeader.biSize ||
            DataFormatSize < VideoHeaderSize ||
            DataFormatSize > ConnectionFormat -> DataFormat.FormatSize
            ) {

            Status = STATUS_INVALID_PARAMETER;

        }

        //
        // Check that the format is a match for the selected range. 
        //
        else if (
            (ConnectionFormat -> VideoInfoHeader.bmiHeader.biWidth !=
                VIRange -> VideoInfoHeader.bmiHeader.biWidth) ||

            (ConnectionFormat -> VideoInfoHeader.bmiHeader.biHeight !=
                VIRange -> VideoInfoHeader.bmiHeader.biHeight) ||

            (ConnectionFormat -> VideoInfoHeader.bmiHeader.biCompression !=
                VIRange -> VideoInfoHeader.bmiHeader.biCompression)
           ) {

            Status = STATUS_NO_MATCH;

        } else {

            //
            // Compute the minimum size of our buffers to validate against.
            // The image synthesis routines synthesize |biHeight| rows of
            // biWidth pixels in either RGB24 or UYVY.  In order to ensure
            // safe synthesis into the buffer, we need to know how large an
            // image this will produce.
            //
            // I do this explicitly because of the method that the data is
            // synthesized.  A variation of this may or may not be necessary
            // depending on the mechanism the driver in question fills the 
            // capture buffers.  The important thing is to ensure that they
            // aren't overrun during capture.
            //
            ULONG ImageSize;

            if (!MultiplyCheckOverflow (
                (ULONG)ConnectionFormat->VideoInfoHeader.bmiHeader.biWidth,
                (ULONG)abs (ConnectionFormat->
                    VideoInfoHeader.bmiHeader.biHeight),
                &ImageSize
                )) {

                Status = STATUS_INVALID_PARAMETER;
            }

            //
            // We only support KS_BI_RGB (24) and KS_BI_YUV422 (16), so
            // this is valid for those formats.
            //
            else if (!MultiplyCheckOverflow (
                ImageSize,
                (ULONG)(ConnectionFormat->
                    VideoInfoHeader.bmiHeader.biBitCount / 8),
                &ImageSize
                )) {

                Status = STATUS_INVALID_PARAMETER;

            }

            //
            // Valid for the formats we use.  Otherwise, this would be
            // checked later.
            //
            else if (ConnectionFormat->VideoInfoHeader.bmiHeader.biSizeImage <
                    ImageSize) {

                Status = STATUS_INVALID_PARAMETER;

            } else {

                //
                // We can accept the format. 
                //
                Status = STATUS_SUCCESS;
    
                //
                // OldFormat is an indication that this is a format change.  
                // Since I do not implement the 
                // KSPROPERTY_CONNECTION_PROPOSEDATAFORMAT, by default, I do 
                // not handle dynamic format changes.
                //
                // If something changes while we're in the stop state, we're 
                // fine to handle it since we haven't "configured the hardware"
                // yet.
                //
                if (OldFormat) {
                    //
                    // If we're in the stop state, we can handle just about any
                    // change.  We don't support dynamic format changes. 
                    //
                    if (Pin -> DeviceState == KSSTATE_STOP) {
                        if (!VidCapPin -> CaptureVideoInfoHeader ()) {
                            Status = STATUS_INSUFFICIENT_RESOURCES;
                        }
                    } else {
                        //
                        // Because we don't accept dynamic format changes, we
                        // should never get here.  Just being over-protective.
                        //
                        Status = STATUS_INVALID_DEVICE_STATE;
                    }
    
                }
    
            }
    
        }

    }
    
    return Status;

}

/*************************************************/


NTSTATUS
CVideoCapturePin::
Pause (
    IN KSSTATE FromState
    )

/*++

Routine Description:

    Called when the pin transitions into the pause state.  If we're in an 
    upward transition, start the capture DPC.  Note that we do not actually
    trigger capture in the pause state, but we start up our DPC.

Arguments:

    FromState -
        The state that the pin is transitioning away from.  This is either
        KSSTATE_ACQUIRE, indicating an upward transition, or KSSTATE_RUN,
        indicating a downward transition.

Return Value:

    STATUS_SUCCESS

--*/

{

    PAGED_CODE();

    //
    // On the transition from acquire -> pause, start the timer DPC running.
    //
    if (FromState == KSSTATE_ACQUIRE) {
        m_ParentFilter -> StartDPC (m_VideoInfoHeader -> AvgTimePerFrame);
    }

    return STATUS_SUCCESS;

}

/*************************************************/


NTSTATUS
CVideoCapturePin::
Acquire (
    IN KSSTATE FromState
    )

/*++

Routine Description:

    This is called from the base class when the video capture pin transitions
    into the acquire state (from either Stop or Pause).  The state the pin
    transitioned from is passed in.

    During this phase, the video capture pin creates the image synthesizer
    and initializes it.

Arguments:

    FromState -
        The state transitioning from (KSSTATE_STOP or KSSTATE_PAUSE)

Return Value:

    Success / Failure

--*/

{

    PAGED_CODE();

    NT_ASSERT (m_VideoInfoHeader);

    NTSTATUS Status = STATUS_SUCCESS;

    if (FromState == KSSTATE_STOP) {

        m_SynthesisBuffer = reinterpret_cast <PUCHAR> (
            ExAllocatePoolZero (
                NonPagedPool, 
                m_VideoInfoHeader -> bmiHeader.biSizeImage,
                AVSSMP_POOLTAG
                )
            );

        m_BarsBuffer = reinterpret_cast <PUCHAR> (
            ExAllocatePoolZero (
                NonPagedPool,
                m_VideoInfoHeader -> bmiHeader.biSizeImage,
                AVSSMP_POOLTAG
                )
            );

        if (!m_SynthesisBuffer || !m_BarsBuffer) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            //
            // Determine the necessary type of image synthesizer to create 
            // based on the format that has been set on this pin.
            //
            if (m_VideoInfoHeader -> bmiHeader.biBitCount == 24 &&
                m_VideoInfoHeader -> bmiHeader.biCompression == KS_BI_RGB) {
        
                //
                // If we're RGB24, create a new RGB24 synth.  RGB24 surfaces
                // can be in either orientation.  The origin is lower left if
                // height < 0.  Otherwise, it's upper left.
                //
                m_ImageSynth = new (NonPagedPool, 'RysI')
                    CRGB24Synthesizer (
                        m_VideoInfoHeader -> bmiHeader.biHeight >= 0,
                        m_VideoInfoHeader -> bmiHeader.biWidth,
                        ABS (m_VideoInfoHeader -> bmiHeader.biHeight)
                        );
        
            } else
            if (m_VideoInfoHeader -> bmiHeader.biBitCount == 16 &&
                m_VideoInfoHeader -> bmiHeader.biCompression == FOURCC_YUV422) {
        
                //
                // If we're UYVY, create the YUV synth.
                //
                m_ImageSynth = new (NonPagedPool, 'YysI') CYUVSynthesizer (
                    m_VideoInfoHeader -> bmiHeader.biWidth,
                    m_VideoInfoHeader -> bmiHeader.biHeight
                    );
        
            } else
                //
                // We don't synthesize anything but RGB 24 and UYVY.
                //
                Status = STATUS_INVALID_PARAMETER;
        
            if (NT_SUCCESS (Status) && !m_ImageSynth) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
            }

        }
    
        //
        // Bag the image synthesizer.
        //
        if (NT_SUCCESS (Status)) {
            
            Status = KsAddItemToObjectBag (
                m_Pin -> Bag,
                m_ImageSynth,
                reinterpret_cast <PFNKSFREE> (CVideoCapturePin::CleanupSynth)
                );
    
        }

        //
        // If everything is okay at this point, inform the synthesizer of
        // the scratch buffer.
        //
        //
        // avs-vcamera: draw the bars once, into their own buffer, and point
        // the synthesizer at the scratch buffer for the per-frame overlays.
        //
        if (NT_SUCCESS (Status)) {
            m_ImageSynth -> SetBuffer (m_BarsBuffer);
            m_ImageSynth -> SynthesizeBars ();
            m_ImageSynth -> SetBuffer (m_SynthesisBuffer);
        }

    } else {

        //
        // The only other state we can come from is pause.  If we're in a
        // downward state transition below pause, tell the filter to stop the
        // capture DPC.
        //
        m_ParentFilter -> StopDPC ();

    }

    return Status;

}

/*************************************************/


NTSTATUS
CVideoCapturePin::
Stop (
    IN KSSTATE FromState
    )

/*++

Routine Description:

    Called when the video capture pin transitions from acquire to stop.
    This function will clean up the image synth and any data structures
    that we need to clean up on stop.

Arguments:

    FromState -
        The state the pin is transitioning away from.  This should
        always be KSSTATE_ACQUIRE for this call.

Return Value:

    STATUS_SUCCESS

--*/

{
    PAGED_CODE();

    NT_ASSERT (FromState == KSSTATE_ACQUIRE);

    //
    // Remove the image synthesizer from the object bag and free it.
    //
    KsRemoveItemFromObjectBag (
        m_Pin -> Bag,
        m_ImageSynth,
        TRUE
        );

    m_ImageSynth = NULL;

    if (m_SynthesisBuffer) {
        ExFreePool (m_SynthesisBuffer);
        m_SynthesisBuffer = NULL;
    }

    if (m_BarsBuffer) {
        ExFreePool (m_BarsBuffer);
        m_BarsBuffer = NULL;
    }

    return STATUS_SUCCESS;

}

/**************************************************************************

    LOCKED CODE

**************************************************************************/

#ifdef ALLOC_PRAGMA
#pragma code_seg()
#endif // ALLOC_PRAGMA


NTSTATUS
CVideoCapturePin::
CaptureFrame (
    IN PKSPROCESSPIN ProcessPin,
    IN ULONG Tick
    )

/*++

Routine Description:

    This routine is called from the filter processing function to capture
    a frame for the video capture pin.  The process pin to capture to is
    passed.

Arguments:

    ProcessPin -
        The process pin associated with this pin.

    Tick -
        The tick count on the filter.  This is the number of timer DPC's that
        have fired since the timer DPC started.

Return Value:

    STATUS_SUCCESS

--*/

{

    NT_ASSERT (ProcessPin -> Pin == m_Pin);

    //
    // Increment the frame number.  This is the total count of frames which
    // have attempted capture.
    //
    m_FrameNumber++;

    //
    // Since this pin is KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING, it
    // means that we do not require frames available in order to process.
    // This means that this routine can get called from our DPC with no
    // buffers available to capture into.  In this case, we increment our
    // dropped frame counter and do nothing.
    //
    if (ProcessPin -> BytesAvailable) {

        //
        // Because we adjusted the allocator framing, each frame should be
        // sufficient to trigger capture of the appropriate buffer size.
        //
        NT_ASSERT (ProcessPin -> BytesAvailable >= 
            m_VideoInfoHeader -> bmiHeader.biSizeImage);

        //
        // If we get an invalid buffer, kick it out.
        //
        if (ProcessPin -> BytesAvailable < 
            m_VideoInfoHeader -> bmiHeader.biSizeImage) {

            ProcessPin -> BytesUsed = 0;
            ProcessPin -> Terminate = TRUE;
            m_DroppedFrames++;
            return STATUS_SUCCESS;
        }

        //
        // avs-vcamera: start from the cached bars rather than synthesizing
        // them again, then draw the frame counter over them.
        //
        RtlCopyMemory (
            m_SynthesisBuffer,
            m_BarsBuffer,
            m_VideoInfoHeader -> bmiHeader.biSizeImage
            );

        ULONG Width = m_VideoInfoHeader -> bmiHeader.biWidth;
        ULONG Height = ABS (m_VideoInfoHeader -> bmiHeader.biHeight);

        //
        // The frame number as a strip a program can read without OCR: 32
        // blocks across the top of the picture, most significant bit on the
        // left, white for 1 and black for 0.  A reader samples the middle of
        // each block.  Every supported width is a multiple of 64, so blocks
        // are an even number of pixels wide and never split a UYVY pair.
        //
        ULONG Frame = (ULONG)m_FrameNumber;
        ULONG BlockWidth = Width / FRAME_STRIP_BITS;
        ULONG StripHeight = Height / FRAME_STRIP_HEIGHT_DIVISOR;

        for (ULONG Bit = 0; Bit < FRAME_STRIP_BITS; Bit++) {
            BOOLEAN One = (Frame >> (FRAME_STRIP_BITS - 1 - Bit)) & 1;
            m_ImageSynth -> Fill (
                Bit * BlockWidth, 0,
                (Bit + 1) * BlockWidth - 1, StripHeight - 1,
                One ? WHITE : BLACK
                );
        }

        //
        // The same number for a person, large and centred, with the format
        // underneath so a screenshot says which size was being sent.
        //
        ULONG Scale = Height / 120;
        if (Scale == 0) Scale = 1;

        char Text [256];
        Text[0] = '\0';
        RtlStringCbPrintfA(Text, sizeof(Text), "FRAME %06lu", Frame);

        m_ImageSynth -> OverlayText (
            POSITION_CENTER,
            Height / 2 - 4 * Scale,
            Scale,
            Text,
            BLACK,
            WHITE
            );

        RtlStringCbPrintfA(Text, sizeof(Text), "%lux%lu %s  skipped %lu",
            Width, Height,
            m_VideoInfoHeader -> bmiHeader.biCompression == KS_BI_RGB ? "RGB24" : "UYVY",
            m_DroppedFrames);

        m_ImageSynth -> OverlayText (
            POSITION_CENTER,
            Height / 2 + 8 * Scale,
            (Scale + 1) / 2,
            Text,
            BLACK,
            WHITE
            );

        //
        // Copy the synthesized image into the buffer.
        //
        RtlCopyMemory (
            ProcessPin -> Data,
            m_SynthesisBuffer,
            m_VideoInfoHeader -> bmiHeader.biSizeImage
            );
        
        ProcessPin -> BytesUsed = m_VideoInfoHeader -> bmiHeader.biSizeImage;
        ProcessPin -> Terminate = TRUE;


        PKSSTREAM_HEADER StreamHeader = 
            ProcessPin -> StreamPointer -> StreamHeader;

        //
        // If there is a clock assigned to the pin, time stamp the sample.
        //
        if (m_Clock) {

            StreamHeader -> PresentationTime.Time = GetTime ();
            StreamHeader -> Duration = m_VideoInfoHeader -> AvgTimePerFrame;

            StreamHeader -> OptionsFlags =
                KSSTREAM_HEADER_OPTIONSF_TIMEVALID |
                KSSTREAM_HEADER_OPTIONSF_DURATIONVALID;

        }

        //
        // Update the extended header info.
        //
        NT_ASSERT (StreamHeader -> Size >= sizeof (KSSTREAM_HEADER) +
            sizeof (KS_FRAME_INFO));

        //
        // Double check the Stream Header size.  AVStream makes no guarantee
        // that because StreamHeaderSize is set to a specific size that you
        // will get that size.  If the proper data type handlers are not 
        // installed, the stream header will be of default size.
        //
        if (StreamHeader -> Size >= sizeof (KSSTREAM_HEADER) +
            sizeof (KS_FRAME_INFO)) {

            PKS_FRAME_INFO FrameInfo = reinterpret_cast <PKS_FRAME_INFO> (
                StreamHeader + 1
                );
    
            FrameInfo -> ExtendedHeaderSize = sizeof (KS_FRAME_INFO);
            FrameInfo -> PictureNumber = (LONGLONG)m_FrameNumber;
            FrameInfo -> DropCount = (LONGLONG)m_DroppedFrames;

        }
    
    } else {
        m_DroppedFrames++;
    }

    return STATUS_SUCCESS;
        
}

/**************************************************************************

    DESCRIPTOR AND DISPATCH LAYOUT

**************************************************************************/

//
// avs-vcamera: one range per size and format instead of the sample's single
// 320x240 pair.  ksproxy reports each range as one IAMStreamConfig
// capability, so a player sees three sizes in each of two formats.
//
// Each range is a fixed size: input, cropping and output sizes are all the
// range's own dimensions.  A range that advertised a span of output sizes
// would invite a player to ask for sizes in between (MPC-HC does, from a
// preset list), and DispatchSetFormat accepts only the exact dimensions of
// a range.
//
// The smallest size comes first because the first range is the default
// format, and an uncompressed recording at that size is cheap on a guest
// rendering with WARP.  30 fps exactly.
//
#define VCAM_FRAME_INTERVAL 333333

#define VCAM_VIDEO_RANGE(Name, SubTypeGuid, Bpp, Compression, Width, Height) \
const                                                                        \
KS_DATARANGE_VIDEO                                                           \
Name = {                                                                     \
    {                                                                        \
        sizeof (KS_DATARANGE_VIDEO),                /* FormatSize */         \
        0,                                          /* Flags */              \
        (Width) * (Height) * (Bpp) / 8,             /* SampleSize */         \
        0,                                          /* Reserved */           \
        STATICGUIDOF (KSDATAFORMAT_TYPE_VIDEO),     /* MEDIATYPE_Video */    \
        SubTypeGuid,                                                         \
        STATICGUIDOF (KSDATAFORMAT_SPECIFIER_VIDEOINFO) /* FORMAT_VideoInfo */ \
    },                                                                       \
    TRUE,               /* bFixedSizeSamples */                              \
    TRUE,               /* bTemporalCompression (all I frames) */            \
    0,                  /* Reserved (was StreamDescriptionFlags) */          \
    0,                  /* Reserved (was MemoryAllocationFlags) */           \
    {                                                                        \
        STATICGUIDOF (KSDATAFORMAT_SPECIFIER_VIDEOINFO),                     \
        0,                          /* AnalogVideoStandard: none */          \
        (Width), (Height),          /* InputSize */                          \
        (Width), (Height),          /* MinCroppingSize */                    \
        (Width), (Height),          /* MaxCroppingSize */                    \
        1, 1,                       /* CropGranularityX, Y */                \
        1, 1,                       /* CropAlignX, Y */                      \
        (Width), (Height),          /* MinOutputSize */                      \
        (Width), (Height),          /* MaxOutputSize */                      \
        1, 1,                       /* OutputGranularityX, Y */              \
        0, 0, 0, 0,                 /* Stretch and shrink taps */            \
        VCAM_FRAME_INTERVAL,        /* MinFrameInterval, 100 ns units */     \
        640000000,                  /* MaxFrameInterval */                   \
        (Bpp) * (Width) * (Height),         /* MinBitsPerSecond (1 fps) */   \
        (Bpp) * 30 * (Width) * (Height)     /* MaxBitsPerSecond */           \
    },                                                                       \
    {                                                                        \
        0, 0, 0, 0,                 /* rcSource */                           \
        0, 0, 0, 0,                 /* rcTarget */                           \
        (Bpp) * 30 * (Width) * (Height),    /* dwBitRate */                  \
        0L,                         /* dwBitErrorRate */                     \
        VCAM_FRAME_INTERVAL,        /* AvgTimePerFrame */                    \
        sizeof (KS_BITMAPINFOHEADER),                                        \
        (Width),                                                             \
        /* Positive: bottom-up RGB24, the DIB default a capture card */      \
        /* sends.  The sample's top-down RGB24 made the AVI mux take a */    \
        /* colour converter.  UYVY is top-down with a positive height. */    \
        (Height),                                                            \
        1,                          /* biPlanes */                           \
        (Bpp),                      /* biBitCount */                         \
        (Compression),              /* biCompression */                      \
        (Width) * (Height) * (Bpp) / 8, /* biSizeImage */                    \
        0, 0, 0, 0                                                           \
    }                                                                        \
};

#define VCAM_GUID_RGB24 \
    0xe436eb7d, 0x524f, 0x11ce, 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70  /* MEDIASUBTYPE_RGB24 */
#define VCAM_GUID_UYVY \
    0x59565955, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71  /* MEDIASUBTYPE_UYVY */

VCAM_VIDEO_RANGE (FormatUYVY_320x240,   VCAM_GUID_UYVY,  16, FOURCC_YUV422, 320, 240)
VCAM_VIDEO_RANGE (FormatRGB24_320x240,  VCAM_GUID_RGB24, 24, KS_BI_RGB,     320, 240)
VCAM_VIDEO_RANGE (FormatUYVY_640x480,   VCAM_GUID_UYVY,  16, FOURCC_YUV422, 640, 480)
VCAM_VIDEO_RANGE (FormatRGB24_640x480,  VCAM_GUID_RGB24, 24, KS_BI_RGB,     640, 480)
VCAM_VIDEO_RANGE (FormatUYVY_1280x720,  VCAM_GUID_UYVY,  16, FOURCC_YUV422, 1280, 720)
VCAM_VIDEO_RANGE (FormatRGB24_1280x720, VCAM_GUID_RGB24, 24, KS_BI_RGB,     1280, 720)

//
// VideoCapturePinDispatch:
//
// This is the dispatch table for the capture pin.  It provides notifications
// about creation, closure, processing, data formats, etc...
//
const
KSPIN_DISPATCH
VideoCapturePinDispatch = {
    CVideoCapturePin::DispatchCreate,       // Pin Create
    NULL,                                   // Pin Close
    NULL,                                   // Pin Process
    NULL,                                   // Pin Reset
    CVideoCapturePin::DispatchSetFormat,    // Pin Set Data Format
    CCapturePin::DispatchSetState,          // Pin Set Device State
    NULL,                                   // Pin Connect
    NULL,                                   // Pin Disconnect
    NULL,                                   // Clock Dispatch
    NULL                                    // Allocator Dispatch
};

//
// VideoCapturePinAllocatorFraming:
//
// This is the simple framing structure for the capture pin.  Note that this
// will be modified via KsEdit when the actual capture format is determined.
//
DECLARE_SIMPLE_FRAMING_EX (
    VideoCapturePinAllocatorFraming,
    STATICGUIDOF (KSMEMORY_TYPE_KERNEL_NONPAGED),
    KSALLOCATOR_REQUIREMENTF_SYSTEM_MEMORY |
        KSALLOCATOR_REQUIREMENTF_PREFERENCES_ONLY,
    2,
    0,
    2 * PAGE_SIZE,
    2 * PAGE_SIZE
    );

//
// VideoCapturePinDataRanges:
//
// This is the list of data ranges supported on the capture pin: three sizes,
// each in UYVY and RGB24.
//
const 
PKSDATARANGE 
VideoCapturePinDataRanges [CAPTURE_PIN_DATA_RANGE_COUNT] = {
    (PKSDATARANGE) &FormatUYVY_320x240,
    (PKSDATARANGE) &FormatRGB24_320x240,
    (PKSDATARANGE) &FormatUYVY_640x480,
    (PKSDATARANGE) &FormatRGB24_640x480,
    (PKSDATARANGE) &FormatUYVY_1280x720,
    (PKSDATARANGE) &FormatRGB24_1280x720,
    };

