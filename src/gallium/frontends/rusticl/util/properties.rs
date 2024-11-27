#[derive(Default)]
pub struct Properties<T> {
    props: Vec<T>,
}

/// This encapsulates a C property array, where the list is 0 terminated.
impl<T> Properties<T> {
    #[allow(clippy::not_unsafe_ptr_arg_deref)]
    pub fn from_ptr_raw(mut p: *const T) -> Vec<T>
    where
        T: Copy + Default + PartialEq,
    {
        let mut res: Vec<T> = Vec::new();

        if !p.is_null() {
            unsafe {
                while *p != T::default() {
                    res.push(*p);
                    res.push(*p.add(1));
                    p = p.add(2);
                }
            }
            res.push(T::default());
        }

        res
    }

    /// Creates a Properties object copying from the supplied pointer.
    ///
    /// It returns `None` if any property is found twice.
    ///
    /// If `p` is null the saved list of properties will be empty. Otherwise it will be 0
    /// terminated.
    pub fn from_ptr(mut p: *const T) -> Option<Self>
    where
        T: Copy + Default + PartialEq,
    {
        let mut res = Self::default();
        if !p.is_null() {
            unsafe {
                while *p != T::default() {
                    // Property lists are expected to be small, so no point in using HashSet or
                    // sorting.
                    if res.props.contains(&*p) {
                        return None;
                    }

                    res.props.push(*p);
                    res.props.push(*p.add(1));

                    // Advance by two as we read through a list of pairs.
                    p = p.add(2);
                }
            }

            // terminate the array
            res.props.push(T::default());
        }

        Some(res)
    }

    /// Returns true when there is no property available.
    pub fn is_empty(&self) -> bool {
        self.props.len() <= 1
    }

    pub fn iter(&self) -> impl Iterator<Item = (&T, &T)> {
        // TODO: use array_chuncks once stabilized
        self.props
            .chunks_exact(2)
            .map(|elems| (&elems[0], &elems[1]))
    }

    /// Returns the amount of key/value pairs available.
    pub fn len(&self) -> usize {
        // only valid lengths are all uneven numbers and 0, so division by 2 gives us always the
        // correct result.
        self.props.len() / 2
    }
}
